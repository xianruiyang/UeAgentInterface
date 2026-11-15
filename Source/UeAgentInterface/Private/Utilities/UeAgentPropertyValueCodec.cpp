// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Utilities/UeAgentPropertySupportProof.h"
#include "Utilities/UeAgentNativeNumberFormat.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTypeDescriptors.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringEncodedTypes.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringReflectionDescriptors.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"

#include "Containers/AnsiString.h"
#include "Containers/Utf8String.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameplayTagContainer.h"
#include "GameplayTagsManager.h"
#include "Metadata/PCGAttributePropertySelector.h"
#include "Misc/PackageName.h"
#include "StructUtils/InstancedStruct.h"
#include "StructUtils/PropertyBag.h"
#include "UObject/AnsiStrProperty.h"
#include "UObject/Class.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/PropertyOptional.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"
#include "UObject/Utf8StrProperty.h"
#include "Utilities/UeAgentCurveJson.h"
#include "Utilities/UeAgentSplineValue.h"
#include "Utilities/UeAgentEnvTraceDataValue.h"
#include "EnvironmentQuery/EnvQueryTypes.h"
#include "Components/SplineComponent.h"
#include "Engine/CollisionProfile.h"
#include "PhysicsEngine/BodyInstance.h"

#include <limits>
#include <type_traits>

namespace UeAgentPropertyValueCodec {
static FString GetValueKindInternal(const FProperty *Property,
                                    bool bIgnoreFixedArray);

namespace {
double CanonicalNumber(const double Value) {
  // Capture must preserve the native value. Equality tolerances are not a
  // license to round authored data (in particular small nonzero doubles) away.
  return Value == 0.0 ? 0.0 : Value;
}

TSharedPtr<FJsonValue> NativeFloatingNumber(double Value, bool bFloat32) {
  return bFloat32
      ? UeAgentNumericFormatting::MakeNativeValue(static_cast<float>(Value))
      : UeAgentNumericFormatting::MakeNativeValue(Value);
}

TSharedPtr<FJsonValue> NativeNumericValue(const FNumericProperty* Property, double Value) {
  return Property && Property->IsFloatingPoint()
      ? NativeFloatingNumber(Value, CastField<FFloatProperty>(Property) != nullptr)
      : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(Value));
}

constexpr const TCHAR *LiteralField = TEXT("literal");
constexpr const TCHAR *TransformVectorComparison = TEXT("native_transform_vector");

bool IsDirectRoot(const FOptions& Options, int32 Depth) {
  return Options.DirectAuthoringLiteral && Depth == Options.DirectAuthoringDepth;
}

void SelectDestination(const FOptions& Options, int32 Depth, EAuthoringValueDestination Destination) {
  if (IsDirectRoot(Options, Depth) && Options.DirectAuthoringDestination)
    *Options.DirectAuthoringDestination = Destination;
}

FValueComparison CompareTransformVector(const FVector& Expected, const FVector& Actual,
    const TSharedPtr<FJsonValue>& ExpectedValue, const TSharedPtr<FJsonValue>& ActualValue) {
  // Match FTransform's native translation/scale equality. SetWorldTransform can
  // compute target-current and current+delta even without an attachment parent.
  // Keep the captured numbers intact; this is only an intent verification rule.
  constexpr double Tolerance = static_cast<double>(KINDA_SMALL_NUMBER);
  const auto& ExpectedComponents = ExpectedValue->Type == EJson::Array
      ? ExpectedValue->AsArray() : ExpectedValue->AsObject()->GetArrayField(TEXT("components"));
  const auto& ActualComponents = ActualValue->Type == EJson::Array
      ? ActualValue->AsArray() : ActualValue->AsObject()->GetArrayField(TEXT("components"));
  for (int32 Index = 0; Index < 3; ++Index) {
    if (FMath::Abs(Expected[Index] - Actual[Index]) > Tolerance) {
      const FString Path = (ExpectedValue->Type == EJson::Array ? FString() : FString(TEXT("/components")))
          + TEXT("/") + LexToString(Index);
      return FValueComparison::Different(Path, ExpectedComponents[Index], ActualComponents[Index]);
    }
  }
  return FValueComparison();
}

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

template<typename T>
TSharedPtr<FJsonValue> MakeBuiltinScalarField(const TCHAR* Field, const T& Literal, const FOptions& Options, int32 Depth) {
  if (IsDirectRoot(Options, Depth)) {
    SelectDestination(Options, Depth, EAuthoringValueDestination::Scalar);
    using namespace UE::UAI::StateSemantic;
    // This explicit export consumer has the same uncounted semantic factory
    // contract as CompleteLevelCanonicalCapture. No usage replay is retained.
    UE::UAI::ContextResource::FUeAgentResourceTracker Resources(false);
    FUeAgentCanonicalValuePtr Scalar; FUeAgentStateSemanticError Error;
    bool bCreated;
    if constexpr (std::is_same_v<T, bool>) bCreated = FUeAgentCanonicalValue::TryBoolean(Literal, Resources, Scalar, Error);
    else bCreated = FUeAgentCanonicalValue::TryString(Literal, Resources, Scalar, Error);
    if (bCreated) bCreated = FUeAgentCanonicalValue::TryObject({{Field, Scalar.ToSharedRef()}},
        Resources, *Options.DirectAuthoringLiteral, Error);
    if (!bCreated && Options.DirectAuthoringLiteralError) *Options.DirectAuthoringLiteralError = MoveTemp(Error);
    return nullptr;
  }
  const auto Object = MakeShared<FJsonObject>();
  if constexpr (std::is_same_v<T, bool>) Object->SetBoolField(Field, Literal);
  else Object->SetStringField(Field, Literal);
  return MakeObjectValue(Object);
}

template<typename T>
TSharedPtr<FJsonValue> MakeBuiltinLiteral(const T& Literal, const FOptions& Options, int32 Depth) {
  return MakeBuiltinScalarField(LiteralField, Literal, Options, Depth);
}

bool TryFormatBuiltinNumber(double Number, bool bFloating, bool bFloat32, FString& Lexeme) {
  if (!FMath::IsFinite(Number)) { Lexeme.Reset(); return false; }
  const bool bFormatted = bFloating
      ? (bFloat32
          ? UeAgentNumericFormatting::TryFormatNative(static_cast<float>(Number == 0.0 ? 0.0 : Number), true, Lexeme)
          : UeAgentNumericFormatting::TryFormatNative(Number == 0.0 ? 0.0 : Number, true, Lexeme))
      : FMath::IsFinite(Number);
  if (!bFloating && bFormatted)
    Lexeme = FString::Printf(TEXT("%.17g"), Number == 0.0 ? 0.0 : Number);
  return bFormatted;
}

TSharedPtr<FJsonValue> MakeBuiltinNumber(const FNumericProperty* Property, double Number,
    const FOptions& Options, int32 Depth) {
  if (!IsDirectRoot(Options, Depth))
    return MakeLiteral(NativeNumericValue(Property, Number));
  if (!FMath::IsFinite(Number) && Options.DirectAuthoringDestination) {
    SelectDestination(Options, Depth, EAuthoringValueDestination::NumericJson);
    return MakeLiteral(NativeNumericValue(Property, Number));
  }
  SelectDestination(Options, Depth, EAuthoringValueDestination::Scalar);
  using namespace UE::UAI::StateSemantic;
  FString Lexeme;
  const bool bFormatted = TryFormatBuiltinNumber(Number, Property->IsFloatingPoint(),
      CastField<FFloatProperty>(Property) != nullptr, Lexeme);
  if (!bFormatted) {
    if (Options.DirectAuthoringLiteralError) {
      *Options.DirectAuthoringLiteralError = FDiagnostic::Failure(TEXT("canonical_json_number_lexeme_invalid"),
          TEXT("The JSON number cannot provide a finite exact numeric spelling."), TEXT("canonical_json"));
      Options.DirectAuthoringLiteralError->SemanticPointer = TEXT("/literal");
      Options.DirectAuthoringLiteralError->Source.JsonPath = TEXT("/literal");
    }
    return nullptr; // The explicitly selected producer never retries JSON.
  }
  UE::UAI::ContextResource::FUeAgentResourceTracker Resources(false);
  FUeAgentCanonicalValuePtr Scalar; FUeAgentStateSemanticError Error;
  // Native floating spelling always retains a decimal marker; ordinary native
  // integers were JSON doubles and also had Decimal in the original converter.
  bool bCreated = FUeAgentCanonicalValue::TryDecimal(Lexeme, Resources, Scalar, Error);
  if (bCreated) bCreated = FUeAgentCanonicalValue::TryObject({{LiteralField, Scalar.ToSharedRef()}},
      Resources, *Options.DirectAuthoringLiteral, Error);
  if (!bCreated && Options.DirectAuthoringLiteralError) *Options.DirectAuthoringLiteralError = MoveTemp(Error);
  return nullptr;
}

TSharedPtr<FJsonValue> MakeBuiltinReferencePath(const FString& Path, const FOptions& Options, int32 Depth) {
  if (!IsDirectRoot(Options, Depth)) return EncodeReferencePath(Path);
  SelectDestination(Options, Depth, EAuthoringValueDestination::Reference);
  using namespace UE::UAI::StateSemantic;
  UE::UAI::ContextResource::FUeAgentResourceTracker Resources(false);
  FUeAgentCanonicalValuePtr Value; FUeAgentStateSemanticError Error;
  const bool bValue = Path.IsEmpty() ? FUeAgentCanonicalValue::TryNull(Resources, Value, Error)
      : FUeAgentCanonicalValue::TryString(Path, Resources, Value, Error);
  if ((!bValue || !FUeAgentCanonicalValue::TryObject({{TEXT("path"), Value.ToSharedRef()}},
      Resources, *Options.DirectAuthoringLiteral, Error)) && Options.DirectAuthoringLiteralError)
    *Options.DirectAuthoringLiteralError = MoveTemp(Error);
  return nullptr;
}

FString BoundString(const FString &Value, const FOptions &Options) {
  return Value;
}

bool Fail(FDiagnostic& OutError, const FString& Code, const FString& Reason) {
  OutError = FDiagnostic::Failure(Code, Reason, TEXT("property_value_decode"));
  return false;
}

bool ValueFailure(FDiagnostic& OutError, const FString& Code, const FString& Reason,
                  const TSharedPtr<FJsonValue>& Actual, const FString& Expected,
                  const FString& Pointer = FString()) {
  Fail(OutError, Code, Reason);
  OutError.ActualValue = Actual;
  OutError.ActualValuePresent = Actual.IsValid();
  OutError.ExpectedValue = MakeShared<FJsonValueString>(Expected);
  // This address belongs to the native value representation. The workspace
  // source map resolves wrappers such as literal/components to authored input.
  OutError.Source.InternalJsonPath = Pointer.IsEmpty() ? TEXT("/") : Pointer;
  return false;
}

FString FieldPointer(const FString& Name) {
  return TEXT("/") + Name.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
}

bool PropertyFailure(FDiagnostic& OutError, const FString& Code, const FString& Reason, const FProperty* Property) {
  ValueFailure(OutError, Code, Reason, Property ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Property->GetCPPType())) : nullptr,
      TEXT("supported reflected property"));
  if (Property) OutError.CandidateName = Property->GetName();
  OutError.SourceKind = TEXT("native_result");
  OutError.Source = {};
  return false;
}

bool OnlyFields(const TSharedPtr<FJsonObject>& Object, const TArray<FString>& Allowed,
                const FString& Code, FDiagnostic& OutError) {
  for (const auto& Field : Object->Values) {
    if (!Allowed.Contains(Field.Key))
      return ValueFailure(OutError, Code, TEXT("The value contains an unsupported field."),
          MakeShared<FJsonValueString>(Field.Key), FString::Join(Allowed, TEXT(", ")), FieldPointer(Field.Key));
  }
  return true;
}

bool RequiredArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field,
    const TArray<TSharedPtr<FJsonValue>>*& Items, const FString& Code, FDiagnostic& OutError) {
  if (Object->TryGetArrayField(Field, Items) && Items) return true;
  return ValueFailure(OutError, Code, TEXT("The value requires an array at this field."),
      Object->TryGetField(Field), TEXT("array"), FieldPointer(Field));
}

TSharedPtr<FJsonValue> NumberFact(double Number) {
  // Non-finite numbers cannot be emitted as JSON numbers.
  if (FMath::IsFinite(Number)) return MakeShared<FJsonValueNumber>(Number);
  return MakeShared<FJsonValueString>(LexToString(Number));
}

bool Fail(FDiagnostic& OutError, FDiagnostic Diagnostic) {
  OutError = MoveTemp(Diagnostic);
  return false;
}

void PrefixValueFailure(FDiagnostic& Error, const FString& Pointer, const FString& Property) {
  UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(Error, Pointer);
  Error.PropertyPath = Property + (Error.PropertyPath.IsEmpty() ? FString() : TEXT(".") + Error.PropertyPath);
}

void RestoreSequenceInput(FDiagnostic& Error) {
  const auto Restore = [](FString& Pointer) {
    if (Pointer == TEXT("/items")) Pointer.Reset();
    else if (Pointer.StartsWith(TEXT("/items/"))) Pointer.RightChopInline(6);
  };
  Restore(Error.Source.InternalJsonPath);
  for (auto& Child : Error.Diagnostics) RestoreSequenceInput(Child);
  if (Error.PublicCause.IsValid()) {
    auto Cause = MakeShared<FDiagnostic>(*Error.PublicCause);
    RestoreSequenceInput(*Cause);
    Error.PublicCause = MoveTemp(Cause);
  }
}

// A type's native storage may be sparse while its effective state is complete.
// Keep this beside the shared codec so nested properties and every asset consumer
// use the same semantics. Ordinary arrays retain their ordered, exact membership.
TOptional<FValueComparison> CompareEffectiveStruct(const UScriptStruct* Struct, const void* Left,
    const void* Right, const FOptions& Options) {
  if (Options.bCompareStoredValues || Struct != FCollisionResponse::StaticStruct()) return {};
  const auto Effective = [](const void* Value) {
    const auto& Response = *static_cast<const FCollisionResponse*>(Value);
    // The editable array is authoritative for authoring. Template transient
    // caches may await Serialize/FixupData; they are not an import invariant.
    // Interpret each side independently on a temporary value, without using
    // the opposite operand to fill defaults or mutating the observed object.
    const auto* Array = FindFProperty<FArrayProperty>(FCollisionResponse::StaticStruct(), TEXT("ResponseArray"));
    FCollisionResponse Decoded;
    Decoded.SetResponsesArray(*Array->ContainerPtrToValuePtr<TArray<FResponseChannel>>(&Response));
    return Decoded.GetResponseContainer();
  };
  const auto Expected = Effective(Left), Actual = Effective(Right);
  const auto* Array = FindFProperty<FArrayProperty>(FCollisionResponse::StaticStruct(), TEXT("ResponseArray"));
  const auto& Authored = *Array->ContainerPtrToValuePtr<TArray<FResponseChannel>>(Left);
  for (int32 Channel = 0; Channel < 32; ++Channel) {
    const auto ExpectedResponse = Expected.GetResponse(static_cast<ECollisionChannel>(Channel));
    const auto ActualResponse = Actual.GetResponse(static_cast<ECollisionChannel>(Channel));
    if (ExpectedResponse == ActualResponse) continue;
    const FName Name = UCollisionProfile::Get()->ReturnChannelNameFromContainerIndex(Channel);
    const auto ResponseValue = [](ECollisionResponse Response) -> TSharedPtr<FJsonValue> {
      return MakeShared<FJsonValueString>(StaticEnum<ECollisionResponse>()->GetNameStringByValue(Response));
    };
    // Let the exported native setter resolve channel redirects and discard
    // unknown names. The private profile lookup is not exported from Engine.
    // Native application is last-write-wins, so locate the last authored match.
    for (int32 Index = Authored.Num() - 1; Index >= 0; --Index) {
      FCollisionResponse Resolved;
      Resolved.SetResponsesArray({Authored[Index]});
      const auto& Channels = *Array->ContainerPtrToValuePtr<TArray<FResponseChannel>>(&Resolved);
      if (Channels.Num() == 1 && Channels[0].Channel == Name)
        return FValueComparison::Different(FString::Printf(TEXT("/fields/ResponseArray/items/%d/fields/Response"), Index),
            ResponseValue(ExpectedResponse), ResponseValue(ActualResponse));
    }
    // A default response has no authored array index. Keep its channel identity
    // with the two effective values instead of inventing an input location.
    const auto ChannelValue = [&](ECollisionResponse Response) -> TSharedPtr<FJsonValue> {
      auto Value = MakeShared<FJsonObject>();
      Value->SetStringField(TEXT("channel"), Name.ToString());
      Value->SetField(TEXT("response"), ResponseValue(Response));
      return MakeShared<FJsonValueObject>(Value);
    };
    return FValueComparison::Different(TEXT("/fields/ResponseArray"), ChannelValue(ExpectedResponse), ChannelValue(ActualResponse));
  }
  return FValueComparison();
}

TOptional<FValueComparison> CompareEffectiveMember(FProperty* Property, const void* Expected,
    const FOptions& Options) {
  if (Options.bCompareStoredValues || !Options.NativeMemberContainer
      || Property->GetOwner<UScriptStruct>() != FCollisionResponse::StaticStruct()
      || Property->GetFName() != TEXT("ResponseArray")) return {};
  auto ExpectedResponse = *static_cast<const FCollisionResponse*>(Options.NativeMemberContainer);
  Property->CopyCompleteValue(Property->ContainerPtrToValuePtr<void>(&ExpectedResponse), Expected);
  auto Compared = CompareEffectiveStruct(FCollisionResponse::StaticStruct(),
      &ExpectedResponse, Options.NativeMemberContainer, Options);
  if (Compared.IsSet() && !Compared->bEquivalent)
    Compared->Path.RemoveFromStart(TEXT("/fields/ResponseArray"));
  return Compared;
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
               TSharedPtr<FJsonObject> &OutObject, FDiagnostic& OutError) {
  if (!Value.IsValid() || Value->Type != EJson::Object) {
    return ValueFailure(OutError, TEXT("expected_object"), TEXT("The property value must be an object."), Value, TEXT("object"));
  }
  OutObject = Value->AsObject();
  return OutObject.IsValid() || Fail(OutError, TEXT("invalid_object"), TEXT("The JSON object has no object storage."));
}

bool GetLiteral(const TSharedPtr<FJsonValue> &Value,
                TSharedPtr<FJsonValue> &OutLiteral, FDiagnostic& OutError) {
  if (!Value.IsValid())
    return ValueFailure(OutError, TEXT("invalid_literal"), TEXT("The literal value is missing."), Value, TEXT("literal value"));
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
    return ValueFailure(OutError, TEXT("missing_literal"), TEXT("The literal envelope requires a literal field."), OutLiteral, TEXT("literal value"), TEXT("/literal"));
  return OnlyFields(Object, {LiteralField}, TEXT("unexpected_literal_fields"), OutError);
}

bool DecodeBitmaskValue(const UEnum* Enum, const TSharedPtr<FJsonValue>& Value,
    uint64 StorageMask, TFunctionRef<bool(int64, uint64&)> ResolveMask,
    uint64& OutRaw, FDiagnostic& OutError) {
  TSharedPtr<FJsonObject> Object;
  const TArray<TSharedPtr<FJsonValue>>* Selected = nullptr;
  if (!GetObject(Value, Object, OutError) ||
      !RequiredArray(Object, TEXT("selected"), Selected, TEXT("missing_bitmask_selected"), OutError) ||
      !OnlyFields(Object, {TEXT("selected"), TEXT("unmapped_bits")}, TEXT("unexpected_bitmask_fields"), OutError)) return false;
  uint64 Raw = 0;
  if (const auto Unmapped = Object->TryGetField(TEXT("unmapped_bits")); Unmapped.IsValid()) {
    if (Unmapped->Type != EJson::String
        || !UE::UAI::StateSemantic::TryParseUInt64Decimal(Unmapped->AsString(), Raw)
        || Raw > StorageMask) return ValueFailure(OutError, TEXT("bitmask_unmapped_bits_out_of_range"),
            TEXT("Unmapped bits must be a decimal uint64 string within the property's storage mask."),
            Unmapped, FString::Printf(TEXT("0..%llu"), StorageMask), TEXT("/unmapped_bits"));
  }
  for (int32 Index = 0; Index < Selected->Num(); ++Index) {
    const auto& Item = (*Selected)[Index];
    const FString Pointer = FString::Printf(TEXT("/selected/%d"), Index);
    if (!Item.IsValid() || Item->Type != EJson::String)
      return ValueFailure(OutError, TEXT("invalid_bitmask_item"), TEXT("Each selected bitmask value must be an enum name."), Item, TEXT("string"), Pointer);
    int64 EnumValue = 0;
    if (!Enum || !UeAgentAuthoringTypeDescriptors::TryResolveEnumValueName(Enum, Item->AsString(), EnumValue))
      return ValueFailure(OutError, TEXT("unknown_bitmask_value"), TEXT("The selected bitmask name is not defined by the enum."),
          Item, Enum ? Enum->GetPathName() : TEXT("a valid bitmask enum"), Pointer);
    uint64 Mask = 0;
    if (!ResolveMask(EnumValue, Mask) || Mask > StorageMask)
      return ValueFailure(OutError, TEXT("bitmask_value_out_of_range:") + Item->AsString(), TEXT("The selected enum value cannot be represented by the property's bitmask storage."),
          Item, FString::Printf(TEXT("mask in 0..%llu"), StorageMask), Pointer);
    Raw |= Mask;
  }
  OutRaw = Raw;
  return true;
}

bool GetReferencePath(const TSharedPtr<FJsonValue> &Value,
                      TSharedPtr<FJsonValue> &OutPath, FDiagnostic& OutError) {
  if (!Value.IsValid())
    return ValueFailure(OutError, TEXT("invalid_reference_path"), TEXT("The object reference path is missing."), Value, TEXT("string or null"));
  if (Value->Type == EJson::String || Value->Type == EJson::Null) {
    OutPath = Value;
    return true;
  }
  TSharedPtr<FJsonObject> Object;
  if (!GetObject(Value, Object, OutError))
    return false;
  OutPath = Object->TryGetField(TEXT("path"));
  if (!OutPath.IsValid())
    return ValueFailure(OutError, TEXT("missing_reference_path"), TEXT("The reference envelope requires a path field."), OutPath, TEXT("string or null"), TEXT("/path"));
  return OnlyFields(Object, {TEXT("path")}, TEXT("unexpected_reference_fields"), OutError);
}

bool GetComponents(const TSharedPtr<FJsonValue> &Value, int32 ExpectedCount,
                   const TArray<TSharedPtr<FJsonValue>> *&OutComponents,
                   FDiagnostic& OutError) {
  if (Value.IsValid() && Value->Type == EJson::Array) {
    OutComponents = &Value->AsArray();
  } else {
    TSharedPtr<FJsonObject> Object;
    if (!GetObject(Value, Object, OutError) ||
        !RequiredArray(Object, TEXT("components"), OutComponents, TEXT("missing_components"), OutError) ||
        !OnlyFields(Object, {TEXT("components")}, TEXT("unexpected_component_fields"), OutError)) return false;
  }
  return OutComponents->Num() == ExpectedCount ||
         ValueFailure(OutError, FString::Printf(TEXT("component_count_mismatch_expected_%d"), ExpectedCount), TEXT("The component array has the wrong number of elements."),
              MakeShared<FJsonValueNumber>(OutComponents->Num()), FString::FromInt(ExpectedCount),
              Value->Type == EJson::Array ? FString() : TEXT("/components"));
}

bool JsonNumber(const TSharedPtr<FJsonValue> &Value, double &OutNumber,
                FDiagnostic& OutError) {
  if (!Value.IsValid() || Value->Type != EJson::Number) {
    return ValueFailure(OutError, TEXT("expected_number"), TEXT("The component or literal must be a number."), Value, TEXT("finite number"));
  }
  FString Lexeme;
  return (UE::UAI::StateSemantic::CanonicalJson::TryGetNumberLexeme(*Value, Lexeme)
          && UE::UAI::StateSemantic::TryParseFiniteDecimal(Lexeme, OutNumber)) ||
         ValueFailure(OutError, TEXT("number_not_finite"), TEXT("The number cannot be represented as a finite value."),
             MakeShared<FJsonValueString>(Lexeme), TEXT("finite number"));
}

bool ValidateFloat32Value(double Number, FDiagnostic& OutError) {
  // Validate the native destination, not the intermediate double spelling:
  // shortest round-trip FLT_MAX (3.4028235e38) is slightly above exact MAX_flt
  // as a double, but narrows to the same finite float32. True overflow remains
  // invalid, consistently with the native conversion used by this codec.
  return (FMath::IsFinite(Number) && FMath::IsFinite(static_cast<float>(Number))) ||
         ValueFailure(OutError, TEXT("float_out_of_range"), TEXT("The number cannot be represented as a finite float32."),
             NumberFact(Number), FString::Printf(TEXT("-%g..%g"), static_cast<double>(MAX_flt), static_cast<double>(MAX_flt)));
}

bool NormalizeQuaternionValue(FQuat &Quat, FDiagnostic& OutError) {
  if (Quat.ContainsNaN())
    return ValueFailure(OutError, TEXT("number_not_finite"), TEXT("Quaternion components must be finite."),
        MakeShared<FJsonValueString>(Quat.ToString()), TEXT("four finite components"));
  const double Scale = FMath::Max(
      FMath::Max(FMath::Abs(Quat.X), FMath::Abs(Quat.Y)),
      FMath::Max(FMath::Abs(Quat.Z), FMath::Abs(Quat.W)));
  if (Scale == 0.0)
    return ValueFailure(OutError, TEXT("zero_quaternion"), TEXT("A zero-length quaternion does not define an orientation."),
        MakeShared<FJsonValueString>(Quat.ToString()), TEXT("non-zero quaternion"));
  // Normalize orientation before narrowing to native float components. Scaling
  // first also avoids overflow/underflow in the native squared-length calculation.
  Quat.X /= Scale;
  Quat.Y /= Scale;
  Quat.Z /= Scale;
  Quat.W /= Scale;
  Quat.Normalize();
  return true;
}

bool JsonInteger(const TSharedPtr<FJsonValue> &Value, int64 Min, int64 Max,
                 int64 &OutNumber, FDiagnostic& OutError) {
  FString Lexeme;
  int64 Number = 0;
  if (!Value || !UE::UAI::StateSemantic::CanonicalJson::TryGetNumberLexeme(*Value, Lexeme) ||
      !UE::UAI::StateSemantic::TryParseInt64Number(Lexeme, Number) || Number < Min || Number > Max) {
    return ValueFailure(OutError, TEXT("integer_out_of_range"), TEXT("The value must be an integer within the property's signed storage range."),
        Value, FString::Printf(TEXT("%lld..%lld"), Min, Max));
  }
  OutNumber = Number;
  return true;
}

bool JsonUnsignedInteger(const TSharedPtr<FJsonValue> &Value, uint64 Max,
                         uint64 &OutNumber, FDiagnostic& OutError) {
  FString Lexeme;
  uint64 Number = 0;
  if (!Value || !UE::UAI::StateSemantic::CanonicalJson::TryGetNumberLexeme(*Value, Lexeme) ||
      !UE::UAI::StateSemantic::TryParseUInt64Number(Lexeme, Number) || Number > Max) {
    return ValueFailure(OutError, TEXT("unsigned_integer_out_of_range"), TEXT("The value must be an integer within the property's unsigned storage range."),
        Value, FString::Printf(TEXT("0..%llu"), Max));
  }
  OutNumber = Number;
  return true;
}

bool JsonNarrowSignedInteger(const FString& Kind, const TSharedPtr<FJsonValue>& Value,
                            int64& Number, FDiagnostic& Error) {
  const int64 Min = Kind == TEXT("int8") ? MIN_int8 : Kind == TEXT("int16") ? MIN_int16 : MIN_int32;
  const int64 Max = Kind == TEXT("int8") ? MAX_int8 : Kind == TEXT("int16") ? MAX_int16 : MAX_int32;
  return JsonInteger(Value, Min, Max, Number, Error);
}

bool JsonNarrowUnsignedInteger(const FString& Kind, const TSharedPtr<FJsonValue>& Value,
                              uint64& Number, FDiagnostic& Error) {
  const uint64 Max = Kind == TEXT("uint8") ? MAX_uint8 : Kind == TEXT("uint16") ? MAX_uint16 : MAX_uint32;
  return JsonUnsignedInteger(Value, Max, Number, Error);
}

bool ValidateNumericConstraints(const FProperty *Property, double Number,
                                FDiagnostic& OutError) {
  if (!FMath::IsFinite(Number))
    return ValueFailure(OutError, TEXT("number_not_finite"), TEXT("The property requires a finite number."), NumberFact(Number), TEXT("finite number"));
  auto ValidateBound = [Property, Number, &OutError](const TCHAR *Key,
                                                     bool bMinimum) {
    const FString Text = Property ? Property->GetMetaData(Key) : FString();
    if (Text.IsEmpty())
      return true;
    double Bound = 0.0;
    if (!LexTryParseString(Bound, *Text) || !FMath::IsFinite(Bound)) {
      ValueFailure(OutError, FString::Printf(TEXT("invalid_%s_metadata"), Key), TEXT("The property's numeric bound metadata is not a finite number."),
          MakeShared<FJsonValueString>(Text), TEXT("finite numeric bound"));
      OutError.PropertyPath = Property->GetName() + TEXT(".metadata.") + Key;
      OutError.SourceKind = TEXT("native_metadata");
      return false;
    }
    if ((bMinimum && Number < Bound) || (!bMinimum && Number > Bound))
      return ValueFailure(OutError, FString::Printf(TEXT("violates_%s"), Key),
          bMinimum ? TEXT("The number is below the property's minimum bound.") : TEXT("The number exceeds the property's maximum bound."),
          NumberFact(Number), FString::Printf(TEXT("%s %s"), bMinimum ? TEXT(">=") : TEXT("<="), *Text));
    return true;
  };
  return ValidateBound(TEXT("ClampMin"), true) &&
         ValidateBound(TEXT("ClampMax"), false);
}

bool ValidateSignedIntegerConstraints(const FProperty *Property, int64 Number,
                                      FDiagnostic& OutError) {
  auto ValidateBound = [Property, Number, &OutError](const TCHAR *Key,
                                                     bool bMinimum) {
    const FString Text = Property ? Property->GetMetaData(Key) : FString();
    if (Text.IsEmpty())
      return true;
    int64 Bound = 0;
    if (UE::UAI::StateSemantic::TryParseInt64Decimal(Text, Bound)) {
      return (bMinimum ? Number >= Bound : Number <= Bound) ||
             ValueFailure(OutError, bMinimum ? TEXT("below_clamp_min") : TEXT("above_clamp_max"),
                 bMinimum ? TEXT("The integer is below the property's minimum bound.") : TEXT("The integer exceeds the property's maximum bound."),
                 MakeShared<FJsonValueString>(LexToString(Number)), FString::Printf(TEXT("%s %s"), bMinimum ? TEXT(">=") : TEXT("<="), *Text));
    }
    return ValidateNumericConstraints(Property, static_cast<double>(Number),
                                      OutError);
  };
  return ValidateBound(TEXT("ClampMin"), true) &&
         ValidateBound(TEXT("ClampMax"), false);
}

bool ValidateUnsignedIntegerConstraints(const FProperty *Property,
                                        uint64 Number, FDiagnostic& OutError) {
  auto ValidateBound = [Property, Number, &OutError](const TCHAR *Key,
                                                     bool bMinimum) {
    const FString Text = Property ? Property->GetMetaData(Key) : FString();
    if (Text.IsEmpty())
      return true;
    uint64 Bound = 0;
    if (UE::UAI::StateSemantic::TryParseUInt64Decimal(Text, Bound)) {
      return (bMinimum ? Number >= Bound : Number <= Bound) ||
             ValueFailure(OutError, bMinimum ? TEXT("below_clamp_min") : TEXT("above_clamp_max"),
                 bMinimum ? TEXT("The integer is below the property's minimum bound.") : TEXT("The integer exceeds the property's maximum bound."),
                 MakeShared<FJsonValueString>(LexToString(Number)), FString::Printf(TEXT("%s %s"), bMinimum ? TEXT(">=") : TEXT("<="), *Text));
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

FString StructKind(const UScriptStruct *Struct) {
  return UeAgentAuthoringTypeDescriptors::GetStructKind(Struct);
}

FProperty *FindStructField(const UScriptStruct *Struct, const TCHAR *Name) {
  return Struct ? FindFProperty<FProperty>(Struct, Name) : nullptr;
}

bool ReadStructNumber(const UScriptStruct *Struct, const void *ValuePtr,
                      const TCHAR *FieldName, double &OutValue,
                      const FNumericProperty** OutProperty = nullptr) {
  FProperty *Field = FindStructField(Struct, FieldName);
  const FNumericProperty *Numeric = CastField<FNumericProperty>(Field);
  if (!Numeric) {
    return false;
  }
  if (OutProperty) *OutProperty = Numeric;
  const void *FieldPtr = Field->ContainerPtrToValuePtr<void>(ValuePtr);
  OutValue =
      Numeric->IsFloatingPoint()
          ? Numeric->GetFloatingPointPropertyValue(FieldPtr)
      : IsUnsignedNumericProperty(Numeric)
          ? static_cast<double>(Numeric->GetUnsignedIntPropertyValue(FieldPtr))
          : static_cast<double>(Numeric->GetSignedIntPropertyValue(FieldPtr));
  return true;
}

struct FObservedBuiltinShape {
  struct FNumber { double Value = 0; bool bFloating = false, bFloat32 = false; };
  TArray<FNumber, TInlineAllocator<9>> Numbers;
  bool bTransform = false, bComplete = false, bFinite = true;
};

bool ObserveBuiltinShapeInternal(const UScriptStruct* Struct, const void* Storage,
    const FString& Kind, FObservedBuiltinShape& Out) {
  Out = {}; if (!Struct || !Storage) return false;
  const auto Named = [&](std::initializer_list<const TCHAR*> Names) {
    for (const auto* Name : Names) {
      double Number = 0; const FNumericProperty* Property = nullptr;
      if (!ReadStructNumber(Struct, Storage, Name, Number, &Property)) return;
      Out.Numbers.Add({Number, Property->IsFloatingPoint(), CastField<FFloatProperty>(Property) != nullptr});
      Out.bFinite &= FMath::IsFinite(Number);
    }
    Out.bComplete = true;
  };
  if (Kind == TEXT("vector2")) Named({TEXT("X"), TEXT("Y")});
  else if (Kind == TEXT("vector3")) Named({TEXT("X"), TEXT("Y"), TEXT("Z")});
  else if (Kind == TEXT("vector4") || Kind == TEXT("quat")) Named({TEXT("X"), TEXT("Y"), TEXT("Z"), TEXT("W")});
  else if (Kind == TEXT("rotator")) Named({TEXT("Pitch"), TEXT("Yaw"), TEXT("Roll")});
  else if (Kind == TEXT("color8") || Kind == TEXT("linear_color")) Named({TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A")});
  else if (Kind == TEXT("transform")) {
    Out.bTransform = true;
    const bool bFloat32 = Struct->GetName() == TEXT("Transform3f");
    const FTransform Transform = bFloat32 ? FTransform(*static_cast<const FTransform3f*>(Storage))
        : *static_cast<const FTransform*>(Storage);
    const FVector Translation = Transform.GetTranslation();
    const FRotator Rotation = Transform.Rotator();
    const FVector Scale = Transform.GetScale3D();
    // Original transform semantics: even Transform3f derives rotator angles in double.
    for (double Number : {Translation.X, Translation.Y, Translation.Z}) Out.Numbers.Add({Number, true, bFloat32});
    for (double Number : {Rotation.Pitch, Rotation.Yaw, Rotation.Roll}) Out.Numbers.Add({Number, true, false});
    for (double Number : {Scale.X, Scale.Y, Scale.Z}) Out.Numbers.Add({Number, true, bFloat32});
    for (const auto& Number : Out.Numbers) Out.bFinite &= FMath::IsFinite(Number.Value);
    Out.bComplete = true;
  } else { Out = {}; return false; }
  return true;
}

TSharedPtr<FJsonValue> EncodeObservedBuiltinShape(const FObservedBuiltinShape& Shape,
    const FOptions& Options, int32 Depth) {
  if (!Shape.bComplete) {
    SelectDestination(Options, Depth, EAuthoringValueDestination::Unavailable);
    return MakeShared<FJsonValueNull>();
  }
  const bool bOriginalNumeric = IsDirectRoot(Options, Depth) && !Shape.bFinite && Options.DirectAuthoringDestination;
  if (bOriginalNumeric) SelectDestination(Options, Depth, EAuthoringValueDestination::NumericJson);
  const bool bDirect = IsDirectRoot(Options, Depth) && !bOriginalNumeric;
  if (bDirect) {
    SelectDestination(Options, Depth, EAuthoringValueDestination::BuiltinShape);
    if (Options.DirectComponentNumbers) *Options.DirectComponentNumbers = Shape.Numbers.Num();
  }
  using namespace UE::UAI::StateSemantic;
  TOptional<UE::UAI::ContextResource::FUeAgentResourceTracker> Resources;
  if (bDirect) Resources.Emplace(false);
  TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields;
  TSharedPtr<FJsonObject> Object;
  if (!bDirect) Object = MakeShared<FJsonObject>();
  FUeAgentStateSemanticError Error;
  const auto Group = [&](const TCHAR* Name, int32 First, int32 Count) {
    TArray<FUeAgentCanonicalValueRef> Values; TArray<TSharedPtr<FJsonValue>> Json;
    for (int32 Index = 0; Index < Count; ++Index) {
      const auto& Number = Shape.Numbers[First + Index];
      if (!bDirect) {
        Json.Add(Number.bFloating ? NativeFloatingNumber(Number.Value, Number.bFloat32)
            : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(Number.Value)));
        continue;
      }
      FString Lexeme; FUeAgentCanonicalValuePtr Value;
      if (!TryFormatBuiltinNumber(Number.Value, Number.bFloating, Number.bFloat32, Lexeme)) {
        if (Options.DirectAuthoringLiteralError) {
          *Options.DirectAuthoringLiteralError = FDiagnostic::Failure(TEXT("canonical_json_number_lexeme_invalid"),
              TEXT("The JSON number cannot provide a finite exact numeric spelling."), TEXT("canonical_json"));
          const FString Path = FString(TEXT("/")) + Name + TEXT("/") + LexToString(Index);
          Options.DirectAuthoringLiteralError->SemanticPointer = Path;
          Options.DirectAuthoringLiteralError->Source.JsonPath = Path;
        }
        return false;
      }
      if (!FUeAgentCanonicalValue::TryDecimal(Lexeme, Resources.GetValue(), Value, Error)) return false;
      Values.Add(Value.ToSharedRef());
    }
    if (!bDirect) { Object->SetArrayField(Name, Json); return true; }
    FUeAgentCanonicalValuePtr Array;
    if (!FUeAgentCanonicalValue::TryArray(Values, Resources.GetValue(), Array, Error)) return false;
    Fields.Emplace(Name, Array.ToSharedRef()); return true;
  };
  const bool bBuilt = Shape.bTransform
      ? Group(TEXT("translation"), 0, 3) && Group(TEXT("rotation"), 3, 3) && Group(TEXT("scale"), 6, 3)
      : Group(TEXT("components"), 0, Shape.Numbers.Num());
  if (!bBuilt) {
    if (!Error.IsEmpty() && Options.DirectAuthoringLiteralError) *Options.DirectAuthoringLiteralError = MoveTemp(Error);
    return nullptr;
  }
  if (!bDirect) return MakeObjectValue(Object);
  if (!FUeAgentCanonicalValue::TryObject(Fields, Resources.GetValue(), *Options.DirectAuthoringLiteral, Error)
      && Options.DirectAuthoringLiteralError) *Options.DirectAuthoringLiteralError = MoveTemp(Error);
  return nullptr;
}

bool WriteStructNumber(const UScriptStruct *Struct, void *ValuePtr,
                       const TCHAR *FieldName, double Value,
                       FDiagnostic& OutError) {
  FProperty *Field = FindStructField(Struct, FieldName);
  FNumericProperty *Numeric = CastField<FNumericProperty>(Field);
  if (!Numeric) {
    return ValueFailure(OutError, FString::Printf(TEXT("missing_numeric_struct_field_%s"), FieldName),
        TEXT("The native struct does not have the required numeric member."),
        MakeShared<FJsonValueString>(Struct ? Struct->GetPathName() : FString()), FieldName);
  }
  void *FieldPtr = Field->ContainerPtrToValuePtr<void>(ValuePtr);
  if (Numeric->IsFloatingPoint()) {
    if (CastField<FFloatProperty>(Numeric) && !ValidateFloat32Value(Value, OutError))
      return false;
    Numeric->SetFloatingPointPropertyValue(FieldPtr, Value);
  } else if (IsUnsignedNumericProperty(Numeric))
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
    const FNumericProperty* Numeric = nullptr;
    if (!ReadStructNumber(StructProperty ? StructProperty->Struct : nullptr,
                          ValuePtr, Name, Number, &Numeric))
      return MakeShared<FJsonValueNull>();
    Components.Add(NativeNumericValue(Numeric, Number));
  }
  TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
  Object->SetArrayField(TEXT("components"), Components);
  return MakeObjectValue(Object);
}

bool DecodeComponents(const UScriptStruct *Struct,
                      const FProperty *ConstraintsProperty,
                      const TSharedPtr<FJsonValue> &Value, void *OutValuePtr,
                      const TArray<const TCHAR *> &Names, FDiagnostic& OutError) {
  const TArray<TSharedPtr<FJsonValue>> *Components = nullptr;
  if (!GetComponents(Value, Names.Num(), Components, OutError))
    return false;
  for (int32 Index = 0; Index < Names.Num(); ++Index) {
    double Number = 0.0;
    if (!JsonNumber((*Components)[Index], Number, OutError) ||
        (ConstraintsProperty &&
         !ValidateNumericConstraints(ConstraintsProperty, Number, OutError)) ||
        !WriteStructNumber(Struct, OutValuePtr, Names[Index], Number, OutError)) {
      PrefixValueFailure(OutError, FString::Printf(TEXT("%s/%d"), Value->Type == EJson::Array ? TEXT("") : TEXT("/components"), Index), Names[Index]);
      return false;
    }
  }
  return true;
}

bool DecodeKnownStructValue(UScriptStruct *Struct,
                            const FProperty *ConstraintsProperty,
                            const TSharedPtr<FJsonValue> &Value,
                            void *OutValuePtr, FDiagnostic& OutError,
                            bool &bOutHandled) {
  bOutHandled = true;
  const FString Kind = StructKind(Struct);
  if (Kind == TEXT("pcg_selector")) {
    TSharedPtr<FJsonValue> Literal;
    if (!GetLiteral(Value, Literal, OutError)) return false;
    if (Literal->Type != EJson::String)
      return ValueFailure(OutError, TEXT("invalid_pcg_selector_text"), TEXT("A PCG selector must be a string literal."),
          Literal, TEXT("string"), Value->Type == EJson::Object ? TEXT("/literal") : FString());
    auto& Selector = *static_cast<FPCGAttributePropertySelector*>(OutValuePtr);
    // Update returns whether the value changed, not whether parsing succeeded.
    // Match native ImportText: empty/unbound selectors remain editable too.
    Selector.Update(Literal->AsString());
    return true;
  }
  if (Kind == TEXT("spline"))
    return UeAgentSplineValue::Decode(Value, *static_cast<FSplineCurves*>(OutValuePtr), OutError);
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
    const TArray<TSharedPtr<FJsonValue>> *Components = nullptr;
    if (!GetComponents(Value, 4, Components, OutError))
      return false;
    double Values[4];
    for (int32 Index = 0; Index < 4; ++Index) {
      if (!JsonNumber((*Components)[Index], Values[Index], OutError) ||
          (ConstraintsProperty &&
           !ValidateNumericConstraints(ConstraintsProperty, Values[Index], OutError))) {
        PrefixValueFailure(OutError, FString::Printf(TEXT("%s/%d"), Value->Type == EJson::Array ? TEXT("") : TEXT("/components"), Index), FString::Printf(TEXT("[%d]"), Index));
        return false;
      }
    }
    FQuat Quat(Values[0], Values[1], Values[2], Values[3]);
    if (!NormalizeQuaternionValue(Quat, OutError)) return false;
    if (Struct->GetName() == TEXT("Quat4f"))
      *static_cast<FQuat4f *>(OutValuePtr) = FQuat4f(Quat);
    else
      *static_cast<FQuat *>(OutValuePtr) = Quat;
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
                             static_cast<double>(Number), OutError)) {
        PrefixValueFailure(OutError, FString::Printf(TEXT("%s/%d"), Value->Type == EJson::Array ? TEXT("") : TEXT("/components"), Index), Names[Index]);
        return false;
      }
    }
    return true;
  }
  if (Kind == TEXT("transform")) {
    TSharedPtr<FJsonObject> Object;
    const TArray<TSharedPtr<FJsonValue>> *Translation = nullptr;
    const TArray<TSharedPtr<FJsonValue>> *Rotation = nullptr;
    const TArray<TSharedPtr<FJsonValue>> *Scale = nullptr;
    if (!GetObject(Value, Object, OutError) ||
        !OnlyFields(Object, {TEXT("translation"), TEXT("rotation"), TEXT("scale")}, TEXT("invalid_transform_shape"), OutError)) return false;
    const auto ReadPart = [&](const TCHAR* Field, const TArray<TSharedPtr<FJsonValue>>*& Components) {
      if (!RequiredArray(Object, Field, Components, TEXT("invalid_transform_shape"), OutError)) return false;
      return Components->Num() == 3 || ValueFailure(OutError, TEXT("invalid_transform_shape"),
          TEXT("Each transform part must have exactly three components."), MakeShared<FJsonValueNumber>(Components->Num()), TEXT("3"), FieldPointer(Field));
    };
    if (!ReadPart(TEXT("translation"), Translation) || !ReadPart(TEXT("rotation"), Rotation) || !ReadPart(TEXT("scale"), Scale)) return false;
    double T[3], R[3], S[3];
    for (int32 Index = 0; Index < 3; ++Index) {
      const auto ReadNumber = [&](const TCHAR* Field, const TSharedPtr<FJsonValue>& Component, double& Number, const bool bFloat32) {
        if (JsonNumber(Component, Number, OutError) && (!bFloat32 || ValidateFloat32Value(Number, OutError))) return true;
        PrefixValueFailure(OutError, FString::Printf(TEXT("/%s/%d"), Field, Index), FString::Printf(TEXT("%s[%d]"), Field, Index));
        return false;
      };
      const bool bFloat32 = Struct->GetName() == TEXT("Transform3f");
      if (!ReadNumber(TEXT("translation"), (*Translation)[Index], T[Index], bFloat32)
          || !ReadNumber(TEXT("rotation"), (*Rotation)[Index], R[Index], false)
          || !ReadNumber(TEXT("scale"), (*Scale)[Index], S[Index], bFloat32)) return false;
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
    if (!GetLiteral(Value, Literal, OutError)) return false;
    return Literal->Type == EJson::String &&
                   FGuid::Parse(Literal->AsString(),
                                *static_cast<FGuid *>(OutValuePtr))
               ? true
               : ValueFailure(OutError, TEXT("invalid_guid"), TEXT("The literal is not a valid GUID string."), Literal, TEXT("GUID string"),
                   Value->Type == EJson::Object ? TEXT("/literal") : FString());
  }
  if (Kind == TEXT("datetime") || Kind == TEXT("timespan")) {
    TSharedPtr<FJsonObject> Object;
    FString TicksText;
    int64 Ticks = 0;
    if (!GetObject(Value, Object, OutError) || !OnlyFields(Object, {TEXT("ticks")}, TEXT("invalid_ticks"), OutError)) return false;
    if (!Object->TryGetStringField(TEXT("ticks"), TicksText) ||
        !UE::UAI::StateSemantic::TryParseInt64Decimal(TicksText, Ticks)) {
      return ValueFailure(OutError, TEXT("invalid_ticks"), TEXT("Ticks must be a decimal int64 string."), Object->TryGetField(TEXT("ticks")), TEXT("decimal int64 string"), TEXT("/ticks"));
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
                           FDiagnostic& OutError) {
  bool bTruncated = false;
  return !(Object.IsValid() &&
           Object->TryGetBoolField(TEXT("truncated"), bTruncated) &&
           bTruncated) ||
         ValueFailure(OutError, TEXT("truncated_value_not_authorable"), TEXT("A truncated value cannot be used as authored input."),
             MakeShared<FJsonValueBoolean>(true), TEXT("complete value"), TEXT("/truncated"));
}

using UeAgentAuthoringTypeDescriptors::IsStructMemberAuthoringEditable;

bool ValidateAuthoringType(const FProperty *Property, const void *ValuePtr,
                           bool bIgnoreFixedArray, bool bRequireEditFlag,
                           FDiagnostic& OutReason, const UScriptStruct* Scope = nullptr) {
  if (!Property) {
    return Fail(OutReason, TEXT("invalid_property"), TEXT("The reflected property is missing."));
  }
  if (bRequireEditFlag && !IsStructMemberAuthoringEditable(Property, Scope)) {
    return Fail(OutReason,
                FString::Printf(TEXT("struct_member_not_editable_%s"), *Property->GetName()),
                FString::Printf(TEXT("The struct member %s is not editable through authoring."), *Property->GetPathName()));
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
    return PropertyFailure(OutReason, TEXT("unsupported_property_type"), TEXT("The reflected property type has no authoring codec."), Property);
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
      return PropertyFailure(OutReason, TEXT("unstable_map_key_type"), TEXT("The map key type does not provide a stable native value hash."), Map->KeyProp);
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
      return Fail(OutReason, TEXT("missing_struct_type"), TEXT("The reflected struct property has no struct type."));
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
        if (!IsStructMemberAuthoringEditable(Field, DynamicStruct))
          continue;
        if (!ValidateAuthoringType(
                Field,
                Field->ContainerPtrToValuePtr<void>(Instanced.GetMemory()),
                false, true, OutReason, DynamicStruct))
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
      if (!IsStructMemberAuthoringEditable(Field, Struct->Struct))
        continue;
      const void *FieldValue =
          ValuePtr ? Field->ContainerPtrToValuePtr<void>(ValuePtr) : nullptr;
      if (!ValidateAuthoringType(Field, FieldValue, false, true, OutReason, Struct->Struct))
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

bool SupportsDirectAuthoringLiteral(const FProperty* Property) {
  if (!Property || Property->ArrayDim != 1) return false;
  if (CastField<FBoolProperty>(Property)
      || CastField<FStrProperty>(Property) || CastField<FAnsiStrProperty>(Property)
      || CastField<FUtf8StrProperty>(Property) || CastField<FNameProperty>(Property)) return true;
  // A bitmask has a structured value and a separate mask/width definition,
  // not a scalar literal. Its own original encoder selects that destination.
  // Invalid native numbers keep their original diagnostic observation.
  return !IsBitmaskProperty(Property) && (GetPropertyEnum(Property)
      || CastField<FNumericProperty>(Property));
}

const UE::UAI::StateSemantic::FUeAgentCanonicalValue* ReadDirectAuthoringLiteral(
    const UE::UAI::StateSemantic::FUeAgentCanonicalValue& Value) {
  using UE::UAI::StateSemantic::EUeAgentValueKind;
  if (Value.GetKind() != EUeAgentValueKind::Object) return nullptr;
  const auto& Fields = Value.GetObjectItems();
  if (Fields.Num() != 1) return nullptr;
  const auto& Scalar = Fields[0].Value.Get();
  if (Fields[0].Key == LiteralField)
    return Scalar.GetKind() == EUeAgentValueKind::Boolean || Scalar.GetKind() == EUeAgentValueKind::String
        || Scalar.GetKind() == EUeAgentValueKind::Decimal ? &Scalar : nullptr;
  return Fields[0].Key == TEXT("selected") && Scalar.GetKind() == EUeAgentValueKind::String ? &Scalar : nullptr;
}

TSharedPtr<FJsonValue> ProjectDirectAuthoringLiteral(const UE::UAI::StateSemantic::FUeAgentCanonicalValue& Value) {
  const auto* Scalar = ReadDirectAuthoringLiteral(Value);
  if (!Scalar) return nullptr;
  if (Scalar->GetKind() == UE::UAI::StateSemantic::EUeAgentValueKind::Decimal)
    return MakeLiteral(MakeShared<FJsonValueNumberString>(Scalar->GetLexeme()));
  return Scalar->GetKind() == UE::UAI::StateSemantic::EUeAgentValueKind::Boolean
      ? MakeBuiltinLiteral(Scalar->GetBoolean(), FOptions{}, 0)
      : MakeBuiltinScalarField(Value.GetObjectItems()[0].Key == TEXT("selected") ? TEXT("selected") : LiteralField,
          Scalar->GetText(), FOptions{}, 0);
}

TSharedPtr<FJsonValue> ProjectDirectAuthoringValue(const UE::UAI::StateSemantic::FUeAgentCanonicalValue& Value) {
  if (ReadDirectAuthoringLiteral(Value)) return ProjectDirectAuthoringLiteral(Value);
  TSharedPtr<FJsonValue> Json; FDiagnostic Error;
  return UE::UAI::StateSemantic::CanonicalJson::TryToJsonValue(Value, Json, Error) ? Json : nullptr;
}

FString GetValueKind(const FProperty *Property) {
  return GetValueKindInternal(Property, false);
}

static bool IsSupportedValue(const FProperty *Property, const void* ValuePtr, FDiagnostic& OutReason,
                             bool* OutContainsText = nullptr, const FStructFieldFilter* FieldFilter = nullptr) {
  TFunction<bool(const FProperty *, const void*, bool)> Validate =
      [&](const FProperty *Candidate, const void* Value, bool bIgnoreFixedArray) {
        if (!Candidate)
          return Fail(OutReason, TEXT("invalid_property"), TEXT("The reflected property is missing."));
        if (OutContainsText && CastField<FTextProperty>(Candidate)) *OutContainsText = true;
        if (!bIgnoreFixedArray && Candidate->ArrayDim > 1) {
          if (!Value) return Validate(Candidate, nullptr, true);
          for (int32 Index = 0; Index < Candidate->ArrayDim; ++Index)
            if (!Validate(Candidate, static_cast<const uint8*>(Value) + Index * Candidate->GetElementSize(), true)) return false;
          return true;
        }
        if (GetValueKindInternal(Candidate, bIgnoreFixedArray) ==
            TEXT("unavailable"))
          return PropertyFailure(OutReason, TEXT("unsupported_property_type"), TEXT("The reflected property type has no authoring codec."), Candidate);
        if (const FArrayProperty *Array = CastField<FArrayProperty>(Candidate)) {
          if (!Value) return Validate(Array->Inner, nullptr, false);
          FScriptArrayHelper Items(Array, Value);
          if (Items.Num() == 0) return Validate(Array->Inner, nullptr, false);
          for (int32 Index = 0; Index < Items.Num(); ++Index)
            if (!Validate(Array->Inner, Items.GetRawPtr(Index), false)) return false;
          return true;
        }
        if (const FSetProperty *Set = CastField<FSetProperty>(Candidate)) {
          if (!Value) return Validate(Set->ElementProp, nullptr, false);
          FScriptSetHelper Items(Set, Value);
          if (Items.Num() == 0) return Validate(Set->ElementProp, nullptr, false);
          for (int32 Index = 0; Index < Items.GetMaxIndex(); ++Index)
            if (Items.IsValidIndex(Index) && !Validate(Set->ElementProp, Items.GetElementPtr(Index), false)) return false;
          return true;
        }
        if (const FMapProperty *Map = CastField<FMapProperty>(Candidate)) {
          if (!Map->KeyProp ||
              !Map->KeyProp->HasAllPropertyFlags(CPF_HasGetValueTypeHash)) {
            return PropertyFailure(OutReason, TEXT("unstable_map_key_type"), TEXT("The map key type does not provide a stable native value hash."), Map->KeyProp);
          }
          if (!Value) return Validate(Map->KeyProp, nullptr, false) && Validate(Map->ValueProp, nullptr, false);
          FScriptMapHelper Items(Map, Value);
          if (Items.Num() == 0) return Validate(Map->KeyProp, nullptr, false) && Validate(Map->ValueProp, nullptr, false);
          for (int32 Index = 0; Index < Items.GetMaxIndex(); ++Index)
            if (Items.IsValidIndex(Index) && (!Validate(Map->KeyProp, Items.GetKeyPtr(Index), false)
                || !Validate(Map->ValueProp, Items.GetValuePtr(Index), false))) return false;
          return true;
        }
        if (const FOptionalProperty *Optional =
                CastField<FOptionalProperty>(Candidate))
          return Validate(Optional->GetValueProperty(), Value && Optional->IsSet(Value)
              ? Optional->GetValuePointerForRead(Value) : nullptr, false);
        if (const FStructProperty *Struct =
                CastField<FStructProperty>(Candidate)) {
          if (!Struct->Struct)
            return Fail(OutReason, TEXT("missing_struct_type"), TEXT("The reflected struct property has no struct type."));
          const UScriptStruct* Members = Struct->Struct;
          const void* Memory = Value;
          const FString Kind = StructKind(Members);
          // A property bag may hold text in its dynamic schema. Do not let the
          // native editor display-string shortcut bypass canonical comparison.
          if (OutContainsText && Kind == TEXT("property_bag")) *OutContainsText = true;
          if (Kind == TEXT("instanced_struct")) {
            if (!Value) return true;
            const auto& Dynamic = *static_cast<const FInstancedStruct*>(Value);
            Members = Dynamic.GetScriptStruct(); Memory = Dynamic.GetMemory();
            if (!Members || !Memory) return true;
          } else if (Kind != TEXT("struct")) return true;
          for (TFieldIterator<FProperty> It(Members,
                                            EFieldIterationFlags::IncludeSuper);
               It; ++It) {
            if ((*It)->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
              continue;
            if (FieldFilter && *FieldFilter && !(*FieldFilter)(Members, *It))
              continue;
            if (!Validate(*It, Memory ? It->ContainerPtrToValuePtr<void>(Memory) : nullptr, false))
              return false;
          }
        }
        return true;
      };
  if (!Validate(Property, ValuePtr, false))
    return false;
  OutReason.Reset();
  return true;
}

bool IsSupported(const FProperty *Property, FDiagnostic& OutReason) {
  return IsSupportedValue(Property, nullptr, OutReason);
}

bool IsSupported(const FProperty *Property, const void* ValuePtr, FDiagnostic& OutReason) {
  return IsSupportedValue(Property, ValuePtr, OutReason);
}

bool IsAuthoringSupported(const FProperty *Property, const void *ValuePtr,
                          FDiagnostic& OutReason) {
  const FStructFieldFilter EditableFields = [](const UScriptStruct* Struct, const FProperty* Field) {
    return IsStructMemberAuthoringEditable(Field, Struct);
  };
  if (!IsSupportedValue(Property, ValuePtr, OutReason, nullptr, &EditableFields) ||
      !ValidateAuthoringType(Property, ValuePtr, false, false, OutReason)) {
    return false;
  }
  OutReason.Reset();
  return true;
}

bool TryGetValueIndependentAuthoringSupport(const FProperty* Property, bool& OutSupported) {
  if (!Property) return false;
  // Exact builtin classes exclude custom FProperty subclasses. Both support
  // walkers stop at these leaves without reading ValuePtr (also for fixed arrays
  // whose every element has the same value-independent support result).
  const FFieldClass* const Leaves[] = {
      FBoolProperty::StaticClass(), FEnumProperty::StaticClass(), FByteProperty::StaticClass(),
      FInt8Property::StaticClass(), FInt16Property::StaticClass(), FIntProperty::StaticClass(), FInt64Property::StaticClass(),
      FUInt16Property::StaticClass(), FUInt32Property::StaticClass(), FUInt64Property::StaticClass(),
      FFloatProperty::StaticClass(), FDoubleProperty::StaticClass(), FNameProperty::StaticClass(),
      FStrProperty::StaticClass(), FAnsiStrProperty::StaticClass(), FUtf8StrProperty::StaticClass(), FTextProperty::StaticClass(),
      FSoftClassProperty::StaticClass(), FSoftObjectProperty::StaticClass(), FClassProperty::StaticClass(),
      FWeakObjectProperty::StaticClass(), FLazyObjectProperty::StaticClass(), FInterfaceProperty::StaticClass(), FObjectProperty::StaticClass()
  };
  bool bIndependent = false;
  for (const FFieldClass* Leaf : Leaves) if (Property->GetClass() == Leaf) { bIndependent = true; break; }
  if (Property->GetClass() == FStructProperty::StaticClass()) {
    const auto* Struct = static_cast<const FStructProperty*>(Property);
    if (Struct->Struct) {
      const FString Kind = StructKind(Struct->Struct);
      bIndependent = Kind != TEXT("struct") && Kind != TEXT("instanced_struct")
          && Kind != TEXT("property_bag") && Kind != TEXT("pcg_selector");
    }
  }
  if (!bIndependent) return false;
  FDiagnostic Reason;
  OutSupported = IsAuthoringSupported(Property, nullptr, Reason);
  return true;
}

bool IsStructFieldAuthoringEditable(const FProperty *Property, const UScriptStruct* Scope) {
  return IsStructMemberAuthoringEditable(Property, Scope);
}

bool ProjectAuthoringValue(const TSharedPtr<FJsonObject>& Type,
                          const TSharedPtr<FJsonValue>& Value,
                          const TSharedPtr<FJsonValue>& Definition,
                          TSharedPtr<FJsonValue>& OutValue,
                          TSharedPtr<FJsonValue>& OutDefinition) {
  OutValue = Value;
  OutDefinition = Definition;
  FString Kind;
  if (!Type || !Type->TryGetStringField(TEXT("kind"), Kind)) return true;
  const auto Schema = Definition && Definition->Type == EJson::Object ? Definition->AsObject() : nullptr;
  const auto Object = Value && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
  if (Kind == TEXT("struct") || Kind == TEXT("instanced_struct") || Kind == TEXT("property_bag")) {
    const TSharedPtr<FJsonObject>* Fields = nullptr;
    // Absence of a captured schema is not evidence of an empty editable shape.
    if (!Schema || !Schema->TryGetObjectField(TEXT("fields"), Fields)) return true;
    const TSharedPtr<FJsonObject>* Values = nullptr;
    if (Object) Object->TryGetObjectField(TEXT("fields"), Values);
    TSharedPtr<FJsonObject> ProjectedFields, ProjectedValues;
    int32 VisitedFields = 0, VisibleFields = 0, VisibleValues = 0;
    using FValueIterator = TMap<FString, TSharedPtr<FJsonValue>>::TConstIterator;
    TOptional<FValueIterator> OriginalValues;
    if (Values) OriginalValues.Emplace((*Values)->Values.CreateConstIterator());
    bool bValueOrderSame = true;
    // Before the first actual change every preceding child is the original
    // child. Copy that prefix only if a changed projection really needs it.
    const auto BeginProjection = [&](int32 PrefixCount) {
      if (ProjectedFields) return;
      ProjectedFields = MakeShared<FJsonObject>();
      ProjectedValues = MakeShared<FJsonObject>();
      int32 PrefixIndex = 0;
      for (const auto& Prefix : (*Fields)->Values) {
        if (PrefixIndex++ == PrefixCount) break;
        ProjectedFields->SetField(Prefix.Key, Prefix.Value);
        if (Values) if (const auto Existing = (*Values)->TryGetField(Prefix.Key))
          ProjectedValues->SetField(Prefix.Key, Existing);
      }
    };
    const auto IncludeField = [&](const auto& Field, int32 Position,
        const TSharedPtr<FJsonValue>& ProjectedValue,
        const TSharedPtr<FJsonValue>& ProjectedDefinition,
        const TSharedPtr<FJsonObject>& Descriptor) {
      ++VisibleFields;
      const auto OriginalValue = Values ? (*Values)->TryGetField(Field.Key) : nullptr;
      const auto OriginalDefinition = Descriptor ? Descriptor->TryGetField(TEXT("value_definition")) : nullptr;
      if (ProjectedValue) {
        ++VisibleValues;
        if (!OriginalValues.IsSet() || !OriginalValues.GetValue()
            || OriginalValues.GetValue().Key().Len() != Field.Key.Len()
            || FMemory::Memcmp(*OriginalValues.GetValue().Key(), *Field.Key, Field.Key.Len() * sizeof(TCHAR)) != 0
            || OriginalValues.GetValue().Value() != ProjectedValue) bValueOrderSame = false;
        if (OriginalValues.IsSet() && OriginalValues.GetValue()) ++OriginalValues.GetValue();
      }
      if (ProjectedValue != OriginalValue
          || (ProjectedDefinition && ProjectedDefinition != OriginalDefinition)
          || !bValueOrderSame) BeginProjection(Position);
      if (!ProjectedFields) return;
      if (Descriptor && ProjectedDefinition && ProjectedDefinition != OriginalDefinition) {
        auto Updated = MakeShared<FJsonObject>(); Updated->Values = Descriptor->Values;
        Updated->SetField(TEXT("value_definition"), ProjectedDefinition);
        ProjectedFields->SetObjectField(Field.Key, Updated);
      } else ProjectedFields->SetField(Field.Key, Field.Value);
      if (ProjectedValue) ProjectedValues->SetField(Field.Key, ProjectedValue);
    };
    for (const auto& Field : (*Fields)->Values) {
      const int32 Position = VisitedFields++;
      const auto Descriptor = Field.Value && Field.Value->Type == EJson::Object ? Field.Value->AsObject() : nullptr;
      bool bEditable = true;
      if (Kind != TEXT("property_bag") && Descriptor
          && Descriptor->TryGetBoolField(TEXT("editable"), bEditable) && !bEditable) {
        BeginProjection(Position);
        continue;
      }
      const TSharedPtr<FJsonObject>* FieldType = nullptr;
      TSharedPtr<FJsonValue> ProjectedValue, ProjectedDefinition;
      const auto FieldValue = Values ? (*Values)->TryGetField(Field.Key) : nullptr;
      if (!Descriptor || !Descriptor->TryGetObjectField(TEXT("type"), FieldType)) {
        IncludeField(Field, Position, FieldValue, nullptr, nullptr);
        continue;
      }
      const bool bFieldActionable = ProjectAuthoringValue(*FieldType, FieldValue,
          Descriptor->TryGetField(TEXT("value_definition")), ProjectedValue, ProjectedDefinition);
      // Bag schema is an explicit desired inventory. Preserve every top-level
      // field and its projected shell even if its descendants have no operations.
      if (!bFieldActionable && Kind != TEXT("property_bag")) { BeginProjection(Position); continue; }
      IncludeField(Field, Position, ProjectedValue, ProjectedDefinition, Descriptor);
    }
    if (Values && VisibleValues != (*Values)->Values.Num()) BeginProjection(VisitedFields);
    if (ProjectedFields) {
      auto ProjectedSchema = MakeShared<FJsonObject>();
      ProjectedSchema->Values = Schema->Values;
      ProjectedSchema->SetObjectField(TEXT("fields"), ProjectedFields);
      OutDefinition = MakeShared<FJsonValueObject>(ProjectedSchema);
      if (Object && Values) {
        auto ProjectedObject = MakeShared<FJsonObject>();
        ProjectedObject->Values = Object->Values;
        ProjectedObject->SetObjectField(TEXT("fields"), ProjectedValues);
        OutValue = MakeShared<FJsonValueObject>(ProjectedObject);
      }
    }
    // Dynamic type selection and bag schema remain actionable with empty fields.
    return Kind != TEXT("struct") || VisibleFields != 0;
  } else if (Kind == TEXT("object_ref")) {
    const TSharedPtr<FJsonObject> *Owned = nullptr, *Fields = nullptr, *OwnedSchema = nullptr;
    const TSharedPtr<FJsonObject> *OwnedFields = nullptr, *PropertiesSchema = nullptr;
    const TArray<TSharedPtr<FJsonValue>> *Properties = nullptr, *Definitions = nullptr;
    // Only captured owned descriptors have paired property schemas. A reference
    // remains actionable even when every property of its instance is hidden.
    if (!Object || !Schema || !Object->TryGetObjectField(TEXT("owned_object"), Owned)
        || !(*Owned)->TryGetArrayField(TEXT("properties"), Properties)
        || !Schema->TryGetObjectField(TEXT("fields"), Fields)
        || !(*Fields)->TryGetObjectField(TEXT("owned_object"), OwnedSchema)
        || !(*OwnedSchema)->TryGetObjectField(TEXT("fields"), OwnedFields)
        || !(*OwnedFields)->TryGetObjectField(TEXT("properties"), PropertiesSchema)
        || !(*PropertiesSchema)->TryGetArrayField(TEXT("items"), Definitions)
        || Properties->Num() != Definitions->Num()) return true;
    TArray<TSharedPtr<FJsonValue>> ProjectedProperties, ProjectedDefinitions;
    for (int32 Index = 0; Index < Properties->Num(); ++Index) {
      const auto PropertyValue = (*Properties)[Index];
      const auto DefinitionValue = (*Definitions)[Index];
      const auto Property = PropertyValue && PropertyValue->Type == EJson::Object ? PropertyValue->AsObject() : nullptr;
      const auto Descriptor = DefinitionValue && DefinitionValue->Type == EJson::Object ? DefinitionValue->AsObject() : nullptr;
      const TSharedPtr<FJsonObject> *PropertyFields = nullptr, *ValueSchema = nullptr, *PropertyType = nullptr;
      if (!Property || !Descriptor || !Descriptor->TryGetObjectField(TEXT("fields"), PropertyFields)
          || !(*PropertyFields)->TryGetObjectField(TEXT("value"), ValueSchema)
          || !(*ValueSchema)->TryGetObjectField(TEXT("type"), PropertyType)) {
        ProjectedProperties.Add((*Properties)[Index]);
        ProjectedDefinitions.Add((*Definitions)[Index]);
        continue;
      }
      TSharedPtr<FJsonValue> ProjectedValue, ProjectedDefinition;
      if (!ProjectAuthoringValue(*PropertyType, Property->TryGetField(TEXT("value")),
          (*ValueSchema)->TryGetField(TEXT("value_definition")), ProjectedValue, ProjectedDefinition)) continue;
      auto CopyWith = [](const TSharedPtr<FJsonObject>& Source, const TCHAR* Name, const TSharedPtr<FJsonValue>& Replacement) {
        auto Copy = MakeShared<FJsonObject>(); Copy->Values = Source->Values;
        if (Replacement) Copy->SetField(Name, Replacement);
        return MakeShared<FJsonValueObject>(Copy);
      };
      ProjectedProperties.Add(CopyWith(Property, TEXT("value"), ProjectedValue));
      const auto UpdatedValueSchema = CopyWith(*ValueSchema, TEXT("value_definition"), ProjectedDefinition);
      const auto UpdatedFields = CopyWith(*PropertyFields, TEXT("value"), UpdatedValueSchema);
      ProjectedDefinitions.Add(CopyWith(Descriptor, TEXT("fields"), UpdatedFields));
    }
    auto CopyObject = [](const TSharedPtr<FJsonObject>& Source) {
      auto Copy = MakeShared<FJsonObject>(); Copy->Values = Source->Values; return Copy;
    };
    auto UpdatedPropertiesSchema = CopyObject(*PropertiesSchema);
    UpdatedPropertiesSchema->SetArrayField(TEXT("items"), ProjectedDefinitions);
    auto UpdatedOwnedFields = CopyObject(*OwnedFields);
    UpdatedOwnedFields->SetObjectField(TEXT("properties"), UpdatedPropertiesSchema);
    auto UpdatedOwnedSchema = CopyObject(*OwnedSchema);
    UpdatedOwnedSchema->SetObjectField(TEXT("fields"), UpdatedOwnedFields);
    auto UpdatedFields = CopyObject(*Fields);
    UpdatedFields->SetObjectField(TEXT("owned_object"), UpdatedOwnedSchema);
    auto UpdatedSchema = CopyObject(Schema);
    UpdatedSchema->SetObjectField(TEXT("fields"), UpdatedFields);
    OutDefinition = MakeShared<FJsonValueObject>(UpdatedSchema);
    auto UpdatedOwned = CopyObject(*Owned);
    UpdatedOwned->SetArrayField(TEXT("properties"), ProjectedProperties);
    auto UpdatedObject = CopyObject(Object);
    UpdatedObject->SetObjectField(TEXT("owned_object"), UpdatedOwned);
    OutValue = MakeShared<FJsonValueObject>(UpdatedObject);
  } else if (Kind == TEXT("map")) {
    const TSharedPtr<FJsonObject> *KeyType = nullptr, *MappedType = nullptr, *Fields = nullptr, *EntriesSchema = nullptr;
    const TArray<TSharedPtr<FJsonValue>> *Entries = nullptr, *Definitions = nullptr;
    if (!Type->TryGetObjectField(TEXT("key_type"), KeyType)
        || !Type->TryGetObjectField(TEXT("mapped_type"), MappedType) || !Object || !Schema
        || !Object->TryGetArrayField(TEXT("entries"), Entries)
        || !Schema->TryGetObjectField(TEXT("fields"), Fields)
        || !(*Fields)->TryGetObjectField(TEXT("entries"), EntriesSchema)
        || !(*EntriesSchema)->TryGetArrayField(TEXT("items"), Definitions)
        || Entries->Num() != Definitions->Num()) return true;
    auto CopyObject = [](const TSharedPtr<FJsonObject>& Source) {
      auto Copy = MakeShared<FJsonObject>(); Copy->Values = Source->Values; return Copy;
    };
    TArray<TSharedPtr<FJsonValue>> ProjectedEntries, ProjectedDefinitions;
    for (int32 Index = 0; Index < Entries->Num(); ++Index) {
      const auto EntryValue = (*Entries)[Index], EntryDefinition = (*Definitions)[Index];
      const auto Entry = EntryValue && EntryValue->Type == EJson::Object ? EntryValue->AsObject() : nullptr;
      const auto EntrySchema = EntryDefinition && EntryDefinition->Type == EJson::Object ? EntryDefinition->AsObject() : nullptr;
      const TSharedPtr<FJsonObject>* EntryFields = nullptr;
      if (!Entry || !EntrySchema || !EntrySchema->TryGetObjectField(TEXT("fields"), EntryFields)) {
        ProjectedEntries.Add(EntryValue); ProjectedDefinitions.Add(EntryDefinition);
        continue;
      }
      auto UpdatedEntry = CopyObject(Entry), UpdatedEntryFields = CopyObject(*EntryFields);
      // Keep both structural shells and the entry's complete from facts even
      // when neither desired key nor mapped value has editable descendants.
      for (const auto& Part : TArray<TPair<FString, TSharedPtr<FJsonObject>>>{
          {TEXT("key"), *KeyType}, {TEXT("value"), *MappedType}}) {
        TSharedPtr<FJsonValue> ProjectedValue, ProjectedDefinition;
        ProjectAuthoringValue(Part.Value, Entry->TryGetField(Part.Key),
            (*EntryFields)->TryGetField(Part.Key), ProjectedValue, ProjectedDefinition);
        if (ProjectedValue) UpdatedEntry->SetField(Part.Key, ProjectedValue);
        if (ProjectedDefinition) UpdatedEntryFields->SetField(Part.Key, ProjectedDefinition);
      }
      auto UpdatedEntrySchema = CopyObject(EntrySchema);
      UpdatedEntrySchema->SetObjectField(TEXT("fields"), UpdatedEntryFields);
      ProjectedEntries.Add(MakeShared<FJsonValueObject>(UpdatedEntry));
      ProjectedDefinitions.Add(MakeShared<FJsonValueObject>(UpdatedEntrySchema));
    }
    auto UpdatedEntriesSchema = CopyObject(*EntriesSchema);
    UpdatedEntriesSchema->SetArrayField(TEXT("items"), ProjectedDefinitions);
    auto UpdatedFields = CopyObject(*Fields);
    UpdatedFields->SetObjectField(TEXT("entries"), UpdatedEntriesSchema);
    auto UpdatedSchema = CopyObject(Schema);
    UpdatedSchema->SetObjectField(TEXT("fields"), UpdatedFields);
    OutDefinition = MakeShared<FJsonValueObject>(UpdatedSchema);
    auto UpdatedObject = CopyObject(Object);
    UpdatedObject->SetArrayField(TEXT("entries"), ProjectedEntries);
    OutValue = MakeShared<FJsonValueObject>(UpdatedObject);
  } else if (Kind == TEXT("optional")) {
    const TSharedPtr<FJsonObject> *InnerType = nullptr, *Fields = nullptr;
    bool bIsSet = false;
    if (!Type->TryGetObjectField(TEXT("inner_type"), InnerType) || !Object || !Schema
        || !Object->TryGetBoolField(TEXT("is_set"), bIsSet) || !bIsSet
        || !Schema->TryGetObjectField(TEXT("fields"), Fields)) return true;
    TSharedPtr<FJsonValue> ProjectedValue, ProjectedDefinition;
    ProjectAuthoringValue(*InnerType, Object->TryGetField(TEXT("value")),
        (*Fields)->TryGetField(TEXT("value")), ProjectedValue, ProjectedDefinition);
    if ((!ProjectedValue || ProjectedValue == Object->TryGetField(TEXT("value")))
        && (!ProjectedDefinition || ProjectedDefinition == (*Fields)->TryGetField(TEXT("value")))) return true;
    auto UpdatedFields = MakeShared<FJsonObject>(); UpdatedFields->Values = (*Fields)->Values;
    if (ProjectedDefinition) UpdatedFields->SetField(TEXT("value"), ProjectedDefinition);
    auto UpdatedSchema = MakeShared<FJsonObject>(); UpdatedSchema->Values = Schema->Values;
    UpdatedSchema->SetObjectField(TEXT("fields"), UpdatedFields);
    OutDefinition = MakeShared<FJsonValueObject>(UpdatedSchema);
    auto UpdatedObject = MakeShared<FJsonObject>(); UpdatedObject->Values = Object->Values;
    if (ProjectedValue) UpdatedObject->SetField(TEXT("value"), ProjectedValue);
    OutValue = MakeShared<FJsonValueObject>(UpdatedObject);
  } else if (Kind == TEXT("array") || Kind == TEXT("fixed_array") || Kind == TEXT("set")) {
    const TSharedPtr<FJsonObject>* ElementType = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* Definitions = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
    if (!Type->TryGetObjectField(TEXT("element_type"), ElementType) || !Schema
        || !Schema->TryGetArrayField(TEXT("items"), Definitions)) return true;
    if (Object) Object->TryGetArrayField(TEXT("items"), Items);
    if (Kind != TEXT("fixed_array") && (!Items || Items->Num() != Definitions->Num())) return true;
    // Dynamic collections retain insertion/removal operations and empty shells.
    // Recursive field projection preserves each item's source/owned identity
    // facts and its matching definition without projecting those facts as fields.
    bool bActionable = Kind != TEXT("fixed_array");
    TArray<TSharedPtr<FJsonValue>> ProjectedItems, ProjectedDefinitions;
    const bool bOutputItems = Object && Items && Items->Num() == Definitions->Num();
    bool bItemsChanged = false, bDefinitionsChanged = false;
    for (int32 Index = 0; Index < Definitions->Num(); ++Index) {
      TSharedPtr<FJsonValue> ProjectedValue, ProjectedDefinition;
      bActionable |= ProjectAuthoringValue(*ElementType,
          Items && Items->IsValidIndex(Index) ? (*Items)[Index] : nullptr, (*Definitions)[Index],
          ProjectedValue, ProjectedDefinition);
      if (bOutputItems) {
        if (!bItemsChanged && ProjectedValue != (*Items)[Index]) {
          bItemsChanged = true;
          ProjectedItems.Append(Items->GetData(), Index);
        }
        if (bItemsChanged) ProjectedItems.Add(ProjectedValue);
      }
      if (!bDefinitionsChanged && ProjectedDefinition != (*Definitions)[Index]) {
        bDefinitionsChanged = true;
        ProjectedDefinitions.Append(Definitions->GetData(), Index);
      }
      if (bDefinitionsChanged) ProjectedDefinitions.Add(ProjectedDefinition);
    }
    if (!bActionable) return false;
    TSharedPtr<FJsonObject> ProjectedSchema;
    const auto EditSchema = [&]() -> FJsonObject& {
      if (!ProjectedSchema) { ProjectedSchema = MakeShared<FJsonObject>(); ProjectedSchema->Values = Schema->Values; }
      return *ProjectedSchema;
    };
    if (bDefinitionsChanged) EditSchema().SetArrayField(TEXT("items"), ProjectedDefinitions);
    if (Schema->HasField(TEXT("element_definition"))) {
      TSharedPtr<FJsonValue> ElementTemplate, ElementDefinition;
      const auto Descriptor = Schema->GetObjectField(TEXT("element_definition"));
      ProjectAuthoringValue(*ElementType, Schema->TryGetField(TEXT("element_template")),
          Descriptor->TryGetField(TEXT("value_definition")), ElementTemplate, ElementDefinition);
      if (ElementDefinition && ElementDefinition != Descriptor->TryGetField(TEXT("value_definition"))) {
        auto ProjectedDescriptor = MakeShared<FJsonObject>(); ProjectedDescriptor->Values = Descriptor->Values;
        ProjectedDescriptor->SetField(TEXT("value_definition"), ElementDefinition);
        EditSchema().SetObjectField(TEXT("element_definition"), ProjectedDescriptor);
      }
      if (ElementTemplate && ElementTemplate != Schema->TryGetField(TEXT("element_template")))
        EditSchema().SetField(TEXT("element_template"), ElementTemplate);
    }
    if (ProjectedSchema) OutDefinition = MakeShared<FJsonValueObject>(ProjectedSchema);
    if (bItemsChanged) {
      auto ProjectedObject = MakeShared<FJsonObject>();
      ProjectedObject->Values = Object->Values;
      ProjectedObject->SetArrayField(TEXT("items"), ProjectedItems);
      OutValue = MakeShared<FJsonValueObject>(ProjectedObject);
    }
  }
  // Dynamic containers, optional presence, owned references and semantic values
  // remain actionable independently of their visible descendants.
  return true;
}

bool RequiresHashSource(const FProperty *Property) {
  return UeAgentAuthoringTypeDescriptors::RequiresHashSource(Property);
}

TSharedPtr<FJsonObject> BuildTypeDescriptor(const FProperty *Property, bool bIgnoreFixedArray) {
  return UeAgentAuthoringTypeDescriptors::BuildJson(Property, bIgnoreFixedArray);
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
                          FDiagnostic& OutError) {
  return ValidateTypeMetadata(Property, Descriptor, OutError, false);
}

bool ValidateTypeMetadata(const FProperty *Property,
                          const TSharedPtr<FJsonObject> &Descriptor,
                          FDiagnostic& OutError, bool bSingleFixedElement) {
  return UeAgentAuthoringTypeDescriptors::ValidateMetadata(Property, Descriptor,
                                                           OutError, bSingleFixedElement);
}

bool TryReadIntegerNumber(const TSharedPtr<FJsonValue>& Value, int64 Min, int64 Max,
                          int64& OutValue, FDiagnostic& OutError) {
  return JsonInteger(Value, Min, Max, OutValue, OutError);
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
    Entry->SetField(Pair.Key, Pair.Value);
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
                                           FDiagnostic& OutError) {
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
           Kind == TEXT("curve") || Kind == TEXT("spline") || Kind == TEXT("instanced_struct") ||
           Kind == TEXT("property_bag") || Kind == TEXT("pcg_selector")) {
    OutType = EPropertyBagPropertyType::Struct;
  } else {
    return ValueFailure(OutError, FString::Printf(TEXT("unsupported_property_bag_value_kind_%s"), *Kind),
        TEXT("The property bag value kind is not supported."), MakeShared<FJsonValueString>(Kind), TEXT("supported property bag kind"), TEXT("/kind"));
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
      const TCHAR* Field = Type->HasTypedField<EJson::String>(TEXT("type_path")) ? TEXT("type_path") :
          Type->HasTypedField<EJson::String>(TEXT("enum_path")) ? TEXT("enum_path") :
          Type->HasTypedField<EJson::String>(TEXT("struct_path")) ? TEXT("struct_path") : TEXT("allowed_class");
      return ValueFailure(OutError, FString::Printf(TEXT("property_bag_type_not_found_%s"), *TypePath),
          TEXT("The property bag type could not be loaded."), MakeShared<FJsonValueString>(TypePath), TEXT("loadable type path"), FieldPointer(Field));
    }
  }
  if ((OutType == EPropertyBagPropertyType::Enum ||
       OutType == EPropertyBagPropertyType::Struct ||
       OutType == EPropertyBagPropertyType::Object ||
       OutType == EPropertyBagPropertyType::SoftObject ||
       OutType == EPropertyBagPropertyType::Class ||
       OutType == EPropertyBagPropertyType::SoftClass) &&
      !OutTypeObject) {
    return ValueFailure(OutError, TEXT("property_bag_type_path_required"), TEXT("The property bag enum, struct, object or class value requires a loadable type path."),
        Type->TryGetField(TEXT("type_path")), Kind + TEXT(" type path"), TEXT("/type_path"));
  }
  return true;
}

static bool ParsePropertyBagSchemaEntry(const TSharedPtr<FJsonObject> &Entry,
                                        FPropertyBagPropertyDesc &OutDesc,
                                        FDiagnostic& OutError) {
  FString Name;
  FString Kind;
  if (!Entry.IsValid() || !Entry->TryGetStringField(TEXT("name"), Name) ||
      Name.IsEmpty() || !Entry->TryGetStringField(TEXT("kind"), Kind) ||
      Kind.IsEmpty()) {
    const FString Field = Name.IsEmpty() ? TEXT("name") : TEXT("kind");
    return ValueFailure(OutError, TEXT("invalid_property_bag_schema_entry"), TEXT("A property bag schema entry requires non-empty name and kind strings."),
        Entry.IsValid() ? Entry->TryGetField(Field) : nullptr, TEXT("non-empty string"), FieldPointer(Field));
  }

  FPropertyBagContainerTypes Containers;
  TSharedPtr<FJsonObject> Current = Entry;
  FString CurrentKind = Kind;
  FString TypePointer;
  while (CurrentKind == TEXT("array") || CurrentKind == TEXT("set")) {
    if (!Containers.Add(CurrentKind == TEXT("array")
                            ? EPropertyBagContainerType::Array
                            : EPropertyBagContainerType::Set)) {
      return ValueFailure(OutError, TEXT("property_bag_container_depth_exceeded"), TEXT("The property bag container nesting exceeds the depth supported by Unreal."),
          MakeShared<FJsonValueString>(CurrentKind), TEXT("supported container nesting"), TypePointer + TEXT("/kind"));
    }
    const TSharedPtr<FJsonObject> *ElementType = nullptr;
    if (!Current->TryGetObjectField(TEXT("element_type"), ElementType) ||
        !ElementType || !ElementType->IsValid() ||
        !(*ElementType)->TryGetStringField(TEXT("kind"), CurrentKind)) {
      return ValueFailure(OutError, TEXT("property_bag_element_type_missing"), TEXT("An array or set schema requires an element_type object with a kind field."),
          Current->TryGetField(TEXT("element_type")), TEXT("object with kind string"), TypePointer + TEXT("/element_type"));
    }
    Current = *ElementType;
    TypePointer += TEXT("/element_type");
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
    if (TopLevelTypePath.IsEmpty() || OutError.Source.InternalJsonPath != TEXT("/type_path"))
      UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(OutError, TypePointer);
    return false;
  }
  OutDesc = FPropertyBagPropertyDesc(FName(*Name), Containers, ValueType,
                                     TypeObject, CPF_Edit);
  return true;
}

static TSharedPtr<FJsonValue>
EncodeStructValueInternal(UScriptStruct *Struct, const void *ValuePtr,
                          UObject *Owner, const FOptions &Options, int32 Depth,
                          TSharedPtr<FJsonObject> *OutValueDefinition = nullptr);

static TSharedPtr<FJsonValue>
EncodeReflectedStructFields(const UScriptStruct *Struct, const void *ValuePtr,
                          UObject *Owner, const FOptions &Options, int32 Depth,
                          TSharedPtr<FJsonObject> *OutValueDefinition);

static TSharedPtr<FJsonObject> CaptureFieldDefinition(FProperty* Field,
    const TSharedPtr<FJsonObject>& ValueDefinition, bool bShareImmutable = false) {
  const auto Descriptor = MakeShared<FJsonObject>();
  if (bShareImmutable) {
    Descriptor->Values = UeAgentAuthoringReflection::CaptureFieldDeclaration(Field)->Values;
    if (ValueDefinition) Descriptor->SetObjectField(TEXT("value_definition"), ValueDefinition);
    return Descriptor;
  }
  Descriptor->SetObjectField(TEXT("type"), BuildTypeDescriptor(Field));
  UeAgentAuthoringReflection::FDescriptor Reflection;
  UeAgentAuthoringReflection::ReadConstraints(Field, Reflection.Constraints);
  if (const auto Constraints = Reflection.MakeConstraintsJson())
    Descriptor->SetObjectField(TEXT("constraints"), Constraints);
  if (ValueDefinition) Descriptor->SetObjectField(TEXT("value_definition"), ValueDefinition);
  if (const auto Candidates = UeAgentAuthoringTypeDescriptors::BuildEnumCandidates(GetPropertyEnum(Field), false, Field))
    for (const auto& Item : Candidates->Values) Descriptor->SetField(Item.Key, Item.Value);
  return Descriptor;
}

static TSharedPtr<FJsonValue> EncodeInternal(FProperty* Property, const void* ValuePtr, UObject* Owner,
    const FOptions& Options, int32 Depth, bool bIgnoreFixedArray,
    TSharedPtr<FJsonObject>* OutValueDefinition = nullptr);

static TSharedPtr<FJsonValue>
EncodeInternalValue(FProperty *Property, const void *ValuePtr, UObject *Owner,
               const FOptions &Options, int32 Depth, bool bIgnoreFixedArray,
               TSharedPtr<FJsonObject> *OutValueDefinition) {
  if (OutValueDefinition) OutValueDefinition->Reset();
  SelectDestination(Options, Depth, EAuthoringValueDestination::OriginalJson);
  if (!Property || !ValuePtr)
    return MakeShared<FJsonValueNull>();
  if (!bIgnoreFixedArray && Property->ArrayDim > 1) {
    TArray<TSharedPtr<FJsonValue>> Items;
    TArray<TSharedPtr<FJsonValue>> Definitions;
    bool bHasDefinition = false;
    for (int32 Index = 0; Index < Property->ArrayDim; ++Index) {
      TSharedPtr<FJsonObject> Definition;
      Items.Add(EncodeInternal(Property,
                               static_cast<const uint8 *>(ValuePtr) +
                                   Index * Property->GetElementSize(),
                               Owner, Options, Depth + 1, true,
                               OutValueDefinition ? &Definition : nullptr));
      if (OutValueDefinition) {
        bHasDefinition |= Definition.IsValid();
        Definitions.Add(Definition.IsValid() ? MakeObjectValue(Definition) : MakeShared<FJsonValueNull>());
      }
    }
    if (OutValueDefinition && bHasDefinition) {
      *OutValueDefinition = MakeShared<FJsonObject>();
      (*OutValueDefinition)->SetArrayField(TEXT("items"), Definitions);
    }
    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetArrayField(TEXT("items"), Items);
    return MakeObjectValue(Object);
  }
  if (const FBoolProperty *Bool = CastField<FBoolProperty>(Property))
    return MakeBuiltinLiteral(Bool->GetPropertyValue(ValuePtr), Options, Depth);
  if (const UEnum *Enum = GetPropertyEnum(Property);
      Enum && !IsBitmaskProperty(Property)) {
    int64 Raw = 0;
    if (const FEnumProperty *EnumProperty = CastField<FEnumProperty>(Property))
      Raw = EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(
          ValuePtr);
    else
      Raw =
          CastFieldChecked<FByteProperty>(Property)->GetPropertyValue(ValuePtr);
    return MakeBuiltinScalarField(TEXT("selected"), EncodeEnumValueName(Enum, Raw), Options, Depth);
  }
  if (const UEnum *Enum = GetPropertyEnum(Property);
      IsBitmaskProperty(Property) && Enum) {
    using namespace UE::UAI::StateSemantic;
    const bool bDirect = IsDirectRoot(Options, Depth);
    TOptional<UE::UAI::ContextResource::FUeAgentResourceTracker> DirectResources;
    TArray<FUeAgentCanonicalValueRef> DirectSelected;
    FUeAgentStateSemanticError DirectError;
    if (bDirect) {
      SelectDestination(Options, Depth, EAuthoringValueDestination::Bitmask);
      DirectResources.Emplace(false);
    }
    TSharedPtr<FJsonObject> Masks;
    if (OutValueDefinition) {
      *OutValueDefinition = MakeShared<FJsonObject>();
      Masks = MakeShared<FJsonObject>();
      // Use the existing value-definition contract envelope, so the same
      // property metadata can be interned/exported as well as compared.
      auto Contract = MakeShared<FJsonObject>();
      Contract->SetObjectField(TEXT("bitmask_masks"), Masks);
      Contract->SetStringField(TEXT("storage_mask"), LexToString(
          UeAgentAuthoringTypeDescriptors::GetIntegerStorageMask(Property)));
      (*OutValueDefinition)->SetStringField(TEXT("type"), TEXT("bitmask"));
      (*OutValueDefinition)->SetObjectField(TEXT("contract"), Contract);
    }
    const FNumericProperty *Numeric = CastField<FNumericProperty>(Property);
    const uint64 StorageMask = UeAgentAuthoringTypeDescriptors::GetIntegerStorageMask(Property);
    const uint64 Raw = (Numeric && IsUnsignedNumericProperty(Numeric)
            ? Numeric->GetUnsignedIntPropertyValue(ValuePtr)
            : static_cast<uint64>(
                  Numeric ? Numeric->GetSignedIntPropertyValue(ValuePtr) : 0)) & StorageMask;
    uint64 Covered = 0;
    TArray<TSharedPtr<FJsonValue>> Selected;
    for (int32 Index = 0; Index < Enum->NumEnums(); ++Index) {
      if (!UeAgentAuthoringTypeDescriptors::IsEnumValueEntry(Enum, Index)) continue;
      const int64 EnumValue = Enum->GetValueByIndex(Index);
      uint64 Mask = 0;
      const bool bValidMask = UeAgentAuthoringTypeDescriptors::TryGetBitmaskMask(Property, Enum, EnumValue, Mask);
      if (Masks && bValidMask)
        Masks->SetStringField(Enum->GetNameStringByIndex(Index), LexToString(Mask));
      if (bValidMask && Mask && (Raw & Mask) == Mask) {
        if (bDirect) {
          FUeAgentCanonicalValuePtr Name;
          if (!FUeAgentCanonicalValue::TryString(Enum->GetNameStringByIndex(Index), DirectResources.GetValue(), Name, DirectError)) {
            if (Options.DirectAuthoringLiteralError) *Options.DirectAuthoringLiteralError = MoveTemp(DirectError);
            return nullptr;
          }
          DirectSelected.Add(Name.ToSharedRef());
        } else Selected.Add(MakeShared<FJsonValueString>(Enum->GetNameStringByIndex(Index)));
        Covered |= Mask;
      }
    }
    if (bDirect) {
      FUeAgentCanonicalValuePtr Items;
      if (!FUeAgentCanonicalValue::TryArray(DirectSelected, DirectResources.GetValue(), Items, DirectError)) {
        if (Options.DirectAuthoringLiteralError) *Options.DirectAuthoringLiteralError = MoveTemp(DirectError);
        return nullptr;
      }
      TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields{{TEXT("selected"), Items.ToSharedRef()}};
      if (const uint64 Unmapped = Raw & ~Covered; Unmapped) {
        FUeAgentCanonicalValuePtr Bits;
        if (!FUeAgentCanonicalValue::TryString(LexToString(Unmapped), DirectResources.GetValue(), Bits, DirectError)) {
          if (Options.DirectAuthoringLiteralError) *Options.DirectAuthoringLiteralError = MoveTemp(DirectError);
          return nullptr;
        }
        Fields.Emplace(TEXT("unmapped_bits"), Bits.ToSharedRef());
      }
      if (!FUeAgentCanonicalValue::TryObject(Fields, DirectResources.GetValue(), *Options.DirectAuthoringLiteral, DirectError)
          && Options.DirectAuthoringLiteralError) *Options.DirectAuthoringLiteralError = MoveTemp(DirectError);
      return nullptr;
    }
    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
    Object->SetArrayField(TEXT("selected"), Selected);
    if (const uint64 Unmapped = Raw & ~Covered; Unmapped)
      Object->SetStringField(TEXT("unmapped_bits"), LexToString(Unmapped));
    return MakeObjectValue(Object);
  }
  if (const FNumericProperty *Numeric = CastField<FNumericProperty>(Property)) {
    if (Numeric->IsFloatingPoint())
      return MakeBuiltinNumber(Numeric, Numeric->GetFloatingPointPropertyValue(ValuePtr), Options, Depth);
    if (GetValueKindInternal(Property, bIgnoreFixedArray) == TEXT("int64"))
      return MakeBuiltinLiteral(LexToString(Numeric->GetSignedIntPropertyValue(ValuePtr)), Options, Depth);
    if (GetValueKindInternal(Property, bIgnoreFixedArray) == TEXT("uint64"))
      return MakeBuiltinLiteral(LexToString(Numeric->GetUnsignedIntPropertyValue(ValuePtr)), Options, Depth);
    return MakeBuiltinNumber(Numeric,
        IsUnsignedNumericProperty(Numeric)
            ? static_cast<double>(
                  Numeric->GetUnsignedIntPropertyValue(ValuePtr))
            : static_cast<double>(
                  Numeric->GetSignedIntPropertyValue(ValuePtr)), Options, Depth);
  }
  if (const FStrProperty *String = CastField<FStrProperty>(Property))
    return MakeBuiltinLiteral(BoundString(String->GetPropertyValue(ValuePtr), Options), Options, Depth);
  if (const FAnsiStrProperty *String = CastField<FAnsiStrProperty>(Property)) {
    const FAnsiString &Source = String->GetPropertyValue(ValuePtr);
    const auto Converted = StringCast<TCHAR>(*Source, Source.Len());
    return MakeBuiltinLiteral(FString(Converted.Length(), Converted.Get()), Options, Depth);
  }
  if (const FUtf8StrProperty *String = CastField<FUtf8StrProperty>(Property)) {
    const FUtf8String &Source = String->GetPropertyValue(ValuePtr);
    const auto Converted = StringCast<TCHAR>(*Source, Source.Len());
    return MakeBuiltinLiteral(FString(Converted.Length(), Converted.Get()), Options, Depth);
  }
  if (const FNameProperty *Name = CastField<FNameProperty>(Property))
    return MakeBuiltinLiteral(BoundString(Name->GetPropertyValue(ValuePtr).ToString(), Options), Options, Depth);
  if (const FTextProperty *Text = CastField<FTextProperty>(Property)) {
    if (OutValueDefinition) {
      *OutValueDefinition = MakeShared<FJsonObject>();
      (*OutValueDefinition)->SetStringField(TEXT("builtin"), TEXT("text"));
    }
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
    const FSoftObjectPath Path =
        Soft->GetPropertyValue(ValuePtr).ToSoftObjectPath();
    if (Options.ScopedReferenceEncoder) {
      SelectDestination(Options, Depth, EAuthoringValueDestination::CallbackJson);
      return Options.ScopedReferenceEncoder(Soft->GetPropertyValue(ValuePtr).Get(), Path.ToString());
    }
    return MakeBuiltinReferencePath(Path.ToString(), Options, Depth);
  }
  if (const FObjectPropertyBase *ObjectProperty =
          CastField<FObjectPropertyBase>(Property)) {
    if (Options.ScopedReferenceEncoder) {
      SelectDestination(Options, Depth, EAuthoringValueDestination::CallbackJson);
      UObject* Referenced = ObjectProperty->GetObjectPropertyValue(ValuePtr);
      return Options.ScopedReferenceEncoder(Referenced, Referenced ? Referenced->GetPathName() : FString());
    }
    TSharedPtr<FJsonValue> CustomValue;
    if (Options.ObjectReferenceEncoder &&
        Options.ObjectReferenceEncoder(ObjectProperty, ValuePtr, Owner, Depth,
                                       CustomValue, OutValueDefinition)) {
      SelectDestination(Options, Depth, EAuthoringValueDestination::CallbackJson);
      return CustomValue.IsValid() ? CustomValue : MakeShared<FJsonValueNull>();
    }
    const UObject* Referenced = ObjectProperty->GetObjectPropertyValue(ValuePtr);
    return MakeBuiltinReferencePath(Referenced ? Referenced->GetPathName() : FString(), Options, Depth);
  }
  if (const FInterfaceProperty *Interface =
          CastField<FInterfaceProperty>(Property)) {
    if (Options.ScopedReferenceEncoder) {
      SelectDestination(Options, Depth, EAuthoringValueDestination::CallbackJson);
      UObject* Referenced = Interface->GetPropertyValue(ValuePtr).GetObject();
      return Options.ScopedReferenceEncoder(Referenced, Referenced ? Referenced->GetPathName() : FString());
    }
    const UObject* Referenced = Interface->GetPropertyValue(ValuePtr).GetObject();
    return MakeBuiltinReferencePath(Referenced ? Referenced->GetPathName() : FString(), Options, Depth);
  }
  if (const FStructProperty *Struct = CastField<FStructProperty>(Property)) {
    const FString Kind = StructKind(Struct->Struct);
    if (Kind == TEXT("curve")) {
      TSharedPtr<FJsonObject> CurveJson;
      return UeAgentCurveJson::TryBuildPropertyCurveJson(Property, ValuePtr,
                                                         CurveJson, OutValueDefinition) &&
                     CurveJson.IsValid()
                 ? MakeObjectValue(CurveJson)
                 : MakeShared<FJsonValueNull>();
    }
    return EncodeStructValueInternal(Struct->Struct, ValuePtr, Owner, Options,
                                     Depth, OutValueDefinition);
  }
  if (const FArrayProperty *Array = CastField<FArrayProperty>(Property)) {
    FScriptArrayHelper Helper(Array, ValuePtr);
    const auto* Inner = Array->Inner;
    // These exact leaf producers have no dynamic definition, callbacks,
    // projection or order normalization. Empty non-Struct/non-owned arrays
    // have no element/template production either. Numeric nonempty arrays
    // keep the original observation and nonfinite diagnostic ordering.
    const bool bImmutableArray = IsDirectRoot(Options, Depth);
    const bool bPureLeaf = bImmutableArray && Inner && Inner->ArrayDim == 1
        && (CastField<FBoolProperty>(Inner) || CastField<FNameProperty>(Inner)
            || CastField<FStrProperty>(Inner) || CastField<FAnsiStrProperty>(Inner)
            || CastField<FUtf8StrProperty>(Inner)
            || (GetPropertyEnum(Inner) && !IsBitmaskProperty(Inner)));
    const bool bEmptyValueOnly = bImmutableArray && Helper.Num() == 0 && Inner
        && !CastField<FStructProperty>(Inner)
        && !(Options.bCaptureOwnedObjectDefinitions
            && UeAgentAuthoringReflection::IsOwnedObjectProperty(Inner));
    if (bPureLeaf || bEmptyValueOnly) {
      SelectDestination(Options, Depth, EAuthoringValueDestination::Sequence);
      using namespace UE::UAI::StateSemantic;
      TArray<FUeAgentCanonicalValueRef> DirectItems; DirectItems.Reserve(Helper.Num());
      for (int32 Index = 0; Index < Helper.Num(); ++Index) {
        FUeAgentCanonicalValuePtr Item; FDiagnostic Error;
        // No leaf in this selected contract reads a callback/struct/ref option.
        // Do not introduce another observable TFunction copy for each array.
        FOptions LeafOptions;
        LeafOptions.DirectAuthoringLiteral = &Item;
        LeafOptions.DirectAuthoringDepth = Depth + 1;
        LeafOptions.DirectAuthoringLiteralError = &Error;
        LeafOptions.bCaptureTypeProvenance = Options.bCaptureTypeProvenance;
        LeafOptions.RetiredTypeRecords = Options.RetiredTypeRecords;
        const auto Json = EncodeInternal(Array->Inner, Helper.GetRawPtr(Index), Owner,
            LeafOptions, Depth + 1, false, nullptr);
        if (!Error.IsEmpty() || !Item || Json) {
          if (Error.IsEmpty()) Error = FDiagnostic::Failure(TEXT("property_array_value_handoff_missing"),
              TEXT("The selected immutable array leaf producer did not supply exactly its value. No alternate encoder was selected."), TEXT("property_value"));
          if (Options.DirectAuthoringLiteralError) *Options.DirectAuthoringLiteralError = MoveTemp(Error);
          return nullptr;
        }
        DirectItems.Add(Item.ToSharedRef());
      }
      UE::UAI::ContextResource::FUeAgentResourceTracker Resources(false);
      FUeAgentCanonicalValuePtr ItemsValue; FUeAgentStateSemanticError Error;
      if ((!FUeAgentCanonicalValue::TryArray(DirectItems, Resources, ItemsValue, Error)
          || !FUeAgentCanonicalValue::TryObject({{TEXT("items"), ItemsValue.ToSharedRef()}},
              Resources, *Options.DirectAuthoringLiteral, Error)) && Options.DirectAuthoringLiteralError)
        *Options.DirectAuthoringLiteralError = MoveTemp(Error);
      return nullptr;
    }
    TArray<TSharedPtr<FJsonValue>> Items;
    TArray<TSharedPtr<FJsonValue>> ItemDefinitions;
    bool bHasItemDefinition = false;
    const int32 Limit = Helper.Num();
    for (int32 Index = 0; Index < Limit; ++Index) {
      TSharedPtr<FJsonObject> ItemDefinition;
      Items.Add(EncodeInternal(Array->Inner, Helper.GetRawPtr(Index), Owner,
                               Options, Depth + 1, false,
                               OutValueDefinition ? &ItemDefinition : nullptr));
      if (OutValueDefinition) {
        bHasItemDefinition |= ItemDefinition.IsValid();
        ItemDefinitions.Add(ItemDefinition.IsValid()
            ? MakeObjectValue(ItemDefinition) : MakeShared<FJsonValueNull>());
      }
    }
    const bool bOwnedElements = Options.bCaptureOwnedObjectDefinitions
        && UeAgentAuthoringReflection::IsOwnedObjectProperty(Array->Inner);
    const bool bStructuredOwnedElements = Options.bCaptureOwnedObjectDefinitions
        && CastField<FStructProperty>(Array->Inner)
        && Array->Inner->HasAnyPropertyFlags(CPF_ContainsInstancedReference);
    TSharedPtr<FJsonObject> ElementDefinition;
    TSharedPtr<FJsonValue> ElementTemplate;
    if (OutValueDefinition && Options.bCaptureStructFieldDescriptors) {
      if (const auto* Element = CastField<FStructProperty>(Array->Inner);
          Element && Element->Struct && !Options.ElementDefinitionStack.Contains(Element->Struct)) {
        FOptions TemplateOptions = Options;
        TemplateOptions.ElementDefinitionStack.Add(Element->Struct);
        // A native default Struct is metadata, never a captured owned instance.
        TemplateOptions.ObjectReferenceEncoder = nullptr;
        TemplateOptions.bCaptureOwnedObjectDefinitions = false;
        FStructOnScope Default(Element->Struct);
        ElementTemplate = EncodeInternal(Array->Inner, Default.GetStructMemory(), nullptr,
            TemplateOptions, Depth + 1, false, &ElementDefinition);
      }
    }
    if (OutValueDefinition && (bHasItemDefinition || bOwnedElements || bStructuredOwnedElements || ElementTemplate)) {
      *OutValueDefinition = MakeShared<FJsonObject>();
      (*OutValueDefinition)->SetArrayField(TEXT("items"), ItemDefinitions);
      if (ElementTemplate) {
        // The descriptor is independent of each captured item's schema/value.
        (*OutValueDefinition)->SetObjectField(TEXT("element_definition"), CaptureFieldDefinition(Array->Inner, ElementDefinition, Options.bShareImmutableFieldDefinitions));
        (*OutValueDefinition)->SetField(TEXT("element_template"), ElementTemplate);
      }
      if (bOwnedElements) (*OutValueDefinition)->SetStringField(TEXT("element_kind"), TEXT("owned_object"));
      else if (bStructuredOwnedElements) (*OutValueDefinition)->SetStringField(TEXT("element_kind"), TEXT("structured_owned_item"));
    }
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
    const bool bSource = Options.StructFieldFilter && RequiresHashSource(Set->ElementProp);
    TArray<TSharedPtr<FJsonValue>> Items;
    TArray<TSharedPtr<FJsonValue>> Definitions;
    bool bHasDefinition = false;
    for (int32 Index = 0; Index < Helper.GetMaxIndex();
         ++Index)
      if (Helper.IsValidIndex(Index)) {
        TSharedPtr<FJsonObject> Definition;
        auto Item = EncodeInternal(Set->ElementProp, Helper.GetElementPtr(Index),
                                 Owner, Options, Depth + 1, false,
                                 OutValueDefinition ? &Definition : nullptr);
        if (bSource && Item && Item->Type == EJson::Object) {
          FOptions Complete = Options; Complete.StructFieldFilter = nullptr;
          Item->AsObject()->SetField(TEXT("from"), EncodeInternal(Set->ElementProp,
              Helper.GetElementPtr(Index), Owner, Complete, Depth + 1, false));
          if (!Definition) Definition = MakeShared<FJsonObject>();
          Definition->SetStringField(TEXT("hash_source"), TEXT("from_required_null_creates"));
        }
        Items.Add(Item);
        if (OutValueDefinition) {
          bHasDefinition |= Definition.IsValid();
          Definitions.Add(Definition.IsValid() ? MakeObjectValue(Definition) : MakeShared<FJsonValueNull>());
        }
      }
    if (OutValueDefinition && bHasDefinition) {
      *OutValueDefinition = MakeShared<FJsonObject>();
      (*OutValueDefinition)->SetArrayField(TEXT("items"), Definitions);
    }
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
    const bool bSource = Options.StructFieldFilter && RequiresHashSource(Map->KeyProp);
    TArray<TSharedPtr<FJsonValue>> Entries;
    TArray<TSharedPtr<FJsonValue>> EntryDefinitions;
    bool bHasEntryDefinition = false;
    for (int32 Index = 0; Index < Helper.GetMaxIndex();
         ++Index) {
      if (!Helper.IsValidIndex(Index))
        continue;
      TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
      TSharedPtr<FJsonObject> KeyDefinition, ValueDefinition;
      Entry->SetField(TEXT("key"),
                      EncodeInternal(Map->KeyProp, Helper.GetKeyPtr(Index),
                                     Owner, Options, Depth + 1, false,
                                     OutValueDefinition ? &KeyDefinition : nullptr));
      Entry->SetField(TEXT("value"),
                      EncodeInternal(Map->ValueProp, Helper.GetValuePtr(Index),
                                     Owner, Options, Depth + 1, false,
                                     OutValueDefinition ? &ValueDefinition : nullptr));
      if (bSource) {
        FOptions Complete = Options; Complete.StructFieldFilter = nullptr;
        Entry->SetField(TEXT("from"), EncodeInternal(Map->KeyProp, Helper.GetKeyPtr(Index),
            Owner, Complete, Depth + 1, false));
      }
      Entries.Add(MakeObjectValue(Entry));
      if (OutValueDefinition) {
        if (KeyDefinition.IsValid() || ValueDefinition.IsValid() || bSource) {
          bHasEntryDefinition = true;
          auto Fields = MakeShared<FJsonObject>();
          if (KeyDefinition.IsValid()) Fields->SetObjectField(TEXT("key"), KeyDefinition);
          if (ValueDefinition.IsValid()) Fields->SetObjectField(TEXT("value"), ValueDefinition);
          auto Definition = MakeShared<FJsonObject>();
          Definition->SetObjectField(TEXT("fields"), Fields);
          if (bSource) Definition->SetStringField(TEXT("hash_source"), TEXT("from_required_null_creates"));
          EntryDefinitions.Add(MakeObjectValue(Definition));
        } else EntryDefinitions.Add(MakeShared<FJsonValueNull>());
      }
    }
    if (OutValueDefinition && bHasEntryDefinition) {
      auto Items = MakeShared<FJsonObject>(); Items->SetArrayField(TEXT("items"), EntryDefinitions);
      auto Fields = MakeShared<FJsonObject>(); Fields->SetObjectField(TEXT("entries"), Items);
      *OutValueDefinition = MakeShared<FJsonObject>();
      (*OutValueDefinition)->SetObjectField(TEXT("fields"), Fields);
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
    if (bSet) {
      TSharedPtr<FJsonObject> Definition;
      Object->SetField(
          TEXT("value"),
          EncodeInternal(Optional->GetValueProperty(),
                         Optional->GetValuePointerForRead(ValuePtr), Owner,
                         Options, Depth + 1, false,
                         OutValueDefinition ? &Definition : nullptr));
      if (OutValueDefinition && Definition.IsValid()) {
        auto Fields = MakeShared<FJsonObject>(); Fields->SetObjectField(TEXT("value"), Definition);
        *OutValueDefinition = MakeShared<FJsonObject>();
        (*OutValueDefinition)->SetObjectField(TEXT("fields"), Fields);
      }
    }
    return MakeObjectValue(Object);
  }
  return MakeShared<FJsonValueNull>();
}

static TSharedPtr<FJsonValue> EncodeInternal(FProperty* Property, const void* ValuePtr, UObject* Owner,
    const FOptions& Options, int32 Depth, bool bIgnoreFixedArray, TSharedPtr<FJsonObject>* OutValueDefinition) {
  if (!UeAgentAuthoringEncodedTypes::FScope::IsActive())
    return EncodeInternalValue(Property, ValuePtr, Owner, Options, Depth, bIgnoreFixedArray, OutValueDefinition);
  if (!Options.bCaptureTypeProvenance) {
    // Preserve the original active-scope definition request, including native
    // element-template construction and observable option-copy order. Only the
    // unused type sidecar is retired, not another encode purpose or callbacks.
    TSharedPtr<FJsonObject> Definition;
    const auto Value = EncodeInternalValue(Property, ValuePtr, Owner, Options, Depth, bIgnoreFixedArray, &Definition);
    if (OutValueDefinition) *OutValueDefinition = Definition;
    if (Options.RetiredTypeRecords && Value && Value->Type != EJson::Null) ++*Options.RetiredTypeRecords;
    return Value;
  }
  TSharedPtr<FJsonObject> Definition;
  const auto Value = EncodeInternalValue(Property, ValuePtr, Owner, Options, Depth, bIgnoreFixedArray, &Definition);
  if (OutValueDefinition) *OutValueDefinition = Definition;
  // Every recursive property retains its type, including when public metadata
  // projection intentionally omits field descriptors.
  if (Value && Value->Type != EJson::Null)
    UeAgentAuthoringEncodedTypes::FScope::Record(Value,
        UeAgentAuthoringTypeDescriptors::Describe(Property, bIgnoreFixedArray), Definition);
  return Value;
}

static TSharedPtr<FJsonValue> EncodeStructValueInternal(UScriptStruct *Struct,
                                                        const void *ValuePtr,
                                                        UObject *Owner,
                                                        const FOptions &Options,
                                                        int32 Depth,
                                                        TSharedPtr<FJsonObject> *OutValueDefinition) {
  if (OutValueDefinition) OutValueDefinition->Reset();
  SelectDestination(Options, Depth, EAuthoringValueDestination::OriginalJson);
  if (!Struct || !ValuePtr) {
    return MakeShared<FJsonValueNull>();
  }

  const FString Kind = StructKind(Struct);
  if (Kind == TEXT("instanced_struct")) {
    const FInstancedStruct &Instanced =
        *static_cast<const FInstancedStruct *>(ValuePtr);
    TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
    const UScriptStruct *DynamicStruct = Instanced.GetScriptStruct();
    if (DynamicStruct && Instanced.GetMemory()) {
      Object = EncodeReflectedStructFields(DynamicStruct, Instanced.GetMemory(),
          Owner, Options, Depth, OutValueDefinition)->AsObject();
      Object->SetStringField(TEXT("struct_path"),
                             DynamicStruct->GetPathName());
    } else {
      Object->SetField(TEXT("struct_path"), MakeShared<FJsonValueNull>());
      Object->SetObjectField(TEXT("fields"), MakeShared<FJsonObject>());
    }
    return MakeObjectValue(Object);
  }
  if (Kind == TEXT("property_bag")) {
    return EncodePropertyBag(
        *static_cast<const FInstancedPropertyBag *>(ValuePtr), Owner, Options,
        Depth, OutValueDefinition);
  }
  if (Kind == TEXT("soft_object_ref")) {
    const FSoftObjectPath &Path =
        *static_cast<const FSoftObjectPath *>(ValuePtr);
    return MakeBuiltinReferencePath(Path.IsNull() ? FString() : Path.ToString(), Options, Depth);
  }
  if (Kind == TEXT("soft_class_ref")) {
    const FSoftClassPath &Path =
        *static_cast<const FSoftClassPath *>(ValuePtr);
    return MakeBuiltinReferencePath(Path.IsNull() ? FString() : Path.ToString(), Options, Depth);
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
  if (Kind == TEXT("pcg_selector"))
    return MakeLiteral(MakeShared<FJsonValueString>(
        static_cast<const FPCGAttributePropertySelector*>(ValuePtr)->ToString()));
  if (Kind == TEXT("spline"))
    return MakeShared<FJsonValueObject>(UeAgentSplineValue::Encode(*static_cast<const FSplineCurves*>(ValuePtr)));
  FObservedBuiltinShape LocalShape;
  if (ObserveBuiltinShapeInternal(Struct, ValuePtr, Kind, LocalShape))
    return EncodeObservedBuiltinShape(LocalShape, Options, Depth);
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

  return EncodeReflectedStructFields(Struct, ValuePtr, Owner, Options, Depth, OutValueDefinition);
}

static TSharedPtr<FJsonValue>
EncodeReflectedStructFields(const UScriptStruct *Struct, const void *ValuePtr,
                          UObject *Owner, const FOptions &Options, int32 Depth,
                          TSharedPtr<FJsonObject> *OutValueDefinition) {
  TOptional<FEnvTraceData> CanonicalTrace;
  if (UeAgentEnvTraceDataValue::IsType(Struct)) {
    CanonicalTrace = UeAgentEnvTraceDataValue::Canonical(*static_cast<const FEnvTraceData*>(ValuePtr));
    ValuePtr = &CanonicalTrace.GetValue();
  }
  TSharedPtr<FJsonObject> Fields = MakeShared<FJsonObject>();
  TSharedPtr<FJsonObject> FieldDefinitions = Options.bCaptureStructFieldDescriptors
      ? TSharedPtr<FJsonObject>(MakeShared<FJsonObject>()) : nullptr;
  for (TFieldIterator<FProperty> It(Struct, EFieldIterationFlags::IncludeSuper);
       It; ++It) {
    FProperty *Field = *It;
    if (Field->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
      continue;
    if (Options.StructFieldFilter && !Options.StructFieldFilter(Struct, Field))
      continue;
    TSharedPtr<FJsonObject> FieldDefinition;
    Fields->SetField(
        Field->GetName(),
        EncodeInternal(Field, Field->ContainerPtrToValuePtr<void>(ValuePtr),
                       Owner, Options, Depth + 1, false,
                       OutValueDefinition ? &FieldDefinition : nullptr));
    if (OutValueDefinition && Options.bCaptureStructFieldDescriptors) {
      FieldDefinition = CaptureFieldDefinition(Field, FieldDefinition, Options.bShareImmutableFieldDefinitions);
      // Complete snapshots retain native state; the authoring projection omits
      // members that the shared struct apply path cannot edit.
      const bool bEditable = IsStructMemberAuthoringEditable(Field, Struct)
          || (Options.StructFieldAuthorization && !Field->HasAnyPropertyFlags(CPF_EditConst)
              && (*Options.StructFieldAuthorization)(Field));
      FieldDefinition->SetBoolField(TEXT("editable"), bEditable);
    }
    if (FieldDefinition.IsValid()) {
      if (!FieldDefinitions.IsValid()) FieldDefinitions = MakeShared<FJsonObject>();
      FieldDefinitions->SetObjectField(Field->GetName(), FieldDefinition);
    }
  }
  if (OutValueDefinition && FieldDefinitions.IsValid()) {
    *OutValueDefinition = MakeShared<FJsonObject>();
    (*OutValueDefinition)->SetObjectField(TEXT("fields"), FieldDefinitions);
  }
  TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
  Object->SetObjectField(TEXT("fields"), Fields);
  return MakeObjectValue(Object);
}

TSharedPtr<FJsonValue> Encode(FProperty *Property, const void *ValuePtr,
                              UObject *Owner, const FOptions &Options,
                              int32 Depth, TSharedPtr<FJsonObject> *OutValueDefinition,
                              bool bIgnoreFixedArray) {
  if (!UeAgentAuthoringEncodedTypes::FScope::IsActive())
    return EncodeInternal(Property, ValuePtr, Owner, Options, Depth, bIgnoreFixedArray, OutValueDefinition);
  FOptions CaptureOptions = Options;
  CaptureOptions.bCaptureStructFieldDescriptors |= OutValueDefinition == nullptr;
  TSharedPtr<FJsonObject> Definition;
  auto Value = EncodeInternal(Property, ValuePtr, Owner, CaptureOptions, Depth, bIgnoreFixedArray, &Definition);
  if (OutValueDefinition) *OutValueDefinition = Definition;
  // EncodeInternal has just recorded this same root. The explicit immutable
  // State consumer owns a fresh output slot and has no intervening reader.
  // Default mutable consumers retain their original public output timing.
  if (Value && Value->Type != EJson::Null && Options.bCaptureTypeProvenance && !IsDirectRoot(Options, Depth))
    UeAgentAuthoringEncodedTypes::FScope::Record(Value,
        UeAgentAuthoringTypeDescriptors::Describe(Property, bIgnoreFixedArray), Definition);
  else if (Value && Value->Type != EJson::Null && !Options.bCaptureTypeProvenance
      && !IsDirectRoot(Options, Depth) && Options.RetiredTypeRecords) ++*Options.RetiredTypeRecords;
  return Value;
}

TSharedPtr<FJsonValue> EncodeStructValue(UScriptStruct *Struct,
                                         const void *ValuePtr, UObject *Owner,
                                         const FOptions &Options, int32 Depth,
                                         TSharedPtr<FJsonObject>* OutValueDefinition) {
  if (!UeAgentAuthoringEncodedTypes::FScope::IsActive())
    return EncodeStructValueInternal(Struct, ValuePtr, Owner, Options, Depth, OutValueDefinition);
  FOptions CaptureOptions = Options;
  CaptureOptions.bCaptureStructFieldDescriptors |= OutValueDefinition == nullptr;
  TSharedPtr<FJsonObject> Definition;
  auto Value = EncodeStructValueInternal(Struct, ValuePtr, Owner, CaptureOptions, Depth, &Definition);
  if (OutValueDefinition) *OutValueDefinition = Definition;
  if (Value && Value->Type != EJson::Null && Options.bCaptureTypeProvenance)
    UeAgentAuthoringEncodedTypes::FScope::Record(Value,
        UeAgentAuthoringTypeDescriptors::DescribeStruct(Struct), Definition);
  else if (Value && Value->Type != EJson::Null && Options.RetiredTypeRecords) ++*Options.RetiredTypeRecords;
  return Value;
}

TSharedPtr<FJsonObject> ProjectNativeDefaultStructFields(
    UScriptStruct* Struct, const void* NativeData, const void* DefaultData,
    const TSharedPtr<FJsonObject>& Fields) {
  if (!Struct || !NativeData || !DefaultData || !Fields) return Fields;
  using namespace UeAgentAuthoringEncodedTypes;
  FScope Projection;
  const auto Complete = MakeShared<FJsonObject>();
  Complete->Values = Fields->Values;
  FScope::Copy(MakeShared<FJsonValueObject>(Fields), MakeShared<FJsonValueObject>(Complete));
  for (const auto& Field : Fields->Values) {
    const FProperty* Property = FindFProperty<FProperty>(Struct, *Field.Key);
    if (Property)
      FScope::SetNativeField(Complete, Field.Key, Field.Value,
          Property->Identical_InContainer(NativeData, DefaultData, PPF_None)
              ? EFieldEmission::NativeDefault : EFieldEmission::Always);
  }
  return Projection.ProjectAuthoringValue(MakeShared<FJsonValueObject>(Complete))->AsObject();
}

static bool DecodeStructValueInternal(UScriptStruct *Struct,
                                      const FProperty *ConstraintsProperty,
                                      const TSharedPtr<FJsonValue> &Value,
                                      void *OutValuePtr, UObject *Owner,
                                      const FOptions &Options,
                                      FDiagnostic& OutError, int32 Depth);

static bool DecodeReflectedStructFields(UScriptStruct *Struct,
                                       const TSharedPtr<FJsonObject>& Fields,
                                       void *OutValuePtr, UObject *Owner,
                                       const FOptions &Options,
                                       FDiagnostic& OutError, int32 Depth,
                                       const TCHAR* Shape);

static bool DecodeInternal(FProperty *Property,
                           const TSharedPtr<FJsonValue> &Value,
                           void *OutValuePtr, UObject *Owner,
                           const FOptions &Options, FDiagnostic& OutError,
                           int32 Depth, bool bIgnoreFixedArray) {
  if (!Property || !OutValuePtr)
    return Fail(OutError, TEXT("invalid_property_or_value_pointer"), TEXT("Property decoding requires a reflected property and writable native value storage."));
  // Like scalar literals and vector components, ordinary sequences may omit
  // their codec envelope. Use the reflected type, not a captured old value,
  // so new/dynamic property-bag fields use the same native container decoder.
  if (Value.IsValid() && Value->Type == EJson::Array) {
    const auto* Struct = CastField<FStructProperty>(Property);
    if ((!bIgnoreFixedArray && Property->ArrayDim > 1) ||
        CastField<FArrayProperty>(Property) || CastField<FSetProperty>(Property) ||
        (Struct && StructKind(Struct->Struct) == TEXT("gameplay_tags"))) {
      const auto Object = MakeShared<FJsonObject>();
      Object->SetArrayField(TEXT("items"), Value->AsArray());
      const bool bDecoded = DecodeInternal(Property, MakeObjectValue(Object), OutValuePtr,
                            Owner, Options, OutError, Depth, bIgnoreFixedArray);
      if (!bDecoded) RestoreSequenceInput(OutError);
      return bDecoded;
    }
  }
  if (!bIgnoreFixedArray && Property->ArrayDim > 1) {
    TSharedPtr<FJsonObject> Object;
    const TArray<TSharedPtr<FJsonValue>> *Items = nullptr;
    if (!GetObject(Value, Object, OutError) ||
        !RequiredArray(Object, TEXT("items"), Items, TEXT("fixed_array_length_mismatch"), OutError) ||
        !OnlyFields(Object, {TEXT("items")}, TEXT("unexpected_fixed_array_fields"), OutError)) return false;
    if (Items->Num() != Property->ArrayDim)
      return ValueFailure(OutError, TEXT("fixed_array_length_mismatch"), TEXT("The fixed array has the wrong number of elements."),
          MakeShared<FJsonValueNumber>(Items->Num()), FString::FromInt(Property->ArrayDim), TEXT("/items"));
    for (int32 Index = 0; Index < Property->ArrayDim; ++Index)
      if (!DecodeInternal(Property, (*Items)[Index],
                          static_cast<uint8 *>(OutValuePtr) +
                              Index * Property->GetElementSize(),
                          Owner, Options, OutError, Depth + 1, true)) {
        PrefixValueFailure(OutError, FString::Printf(TEXT("/items/%d"), Index), FString::Printf(TEXT("[%d]"), Index));
        return false;
      }
    return true;
  }
  if (Options.ScopedValueInitializer
      && !Options.ScopedValueInitializer(Property, Value, OutValuePtr, bIgnoreFixedArray, OutError))
    return false;
  if (const FBoolProperty *Bool = CastField<FBoolProperty>(Property)) {
    TSharedPtr<FJsonValue> Literal;
    if (!GetLiteral(Value, Literal, OutError)) return false;
    if (Literal->Type != EJson::Boolean)
      return ValueFailure(OutError, TEXT("expected_boolean_literal"), TEXT("The boolean literal has the wrong JSON type."),
          Literal, TEXT("boolean"), Value->Type == EJson::Object ? TEXT("/literal") : FString());
    Bool->SetPropertyValue(OutValuePtr, Literal->AsBool());
    return true;
  }
  if (const UEnum *Enum = GetPropertyEnum(Property);
      Enum && !IsBitmaskProperty(Property)) {
    int64 Raw = 0;
    if (!DecodeEnumValue(Enum, Value, Raw, OutError)) return false;
    const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property);
    const FNumericProperty* Underlying = EnumProperty ? EnumProperty->GetUnderlyingProperty() : CastFieldChecked<FByteProperty>(Property);
    const int32 Bits = Underlying->GetElementSize() * 8;
    if (IsUnsignedNumericProperty(Underlying)
        ? Raw < 0 || static_cast<uint64>(Raw) > UeAgentAuthoringTypeDescriptors::GetIntegerStorageMask(Underlying)
        : Bits < 64 && (Raw < -(int64(1) << (Bits - 1)) || Raw > (int64(1) << (Bits - 1)) - 1))
      return ValueFailure(OutError, TEXT("enum_value_out_of_range"), TEXT("The resolved enum value exceeds the property's integer storage range."),
          MakeShared<FJsonValueString>(LexToString(Raw)), FString::Printf(TEXT("%s %d-bit integer"), IsUnsignedNumericProperty(Underlying) ? TEXT("unsigned") : TEXT("signed"), Bits),
          Value->Type == EJson::Object ? TEXT("/selected") : FString());
    if (EnumProperty)
      EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(OutValuePtr,
                                                                 Raw);
    else
      CastFieldChecked<FByteProperty>(Property)->SetPropertyValue(
          OutValuePtr, static_cast<uint8>(Raw));
    return true;
  }
  if (const UEnum *Enum = GetPropertyEnum(Property);
      IsBitmaskProperty(Property) && Enum) {
    uint64 Raw = 0;
    const uint64 StorageMask = UeAgentAuthoringTypeDescriptors::GetIntegerStorageMask(Property);
    if (!DecodeBitmaskValue(Enum, Value, StorageMask,
        [Property, Enum](int64 EnumValue, uint64& Mask) {
          return UeAgentAuthoringTypeDescriptors::TryGetBitmaskMask(Property, Enum, EnumValue, Mask);
        }, Raw, OutError)) return false;
    FNumericProperty *Numeric = CastFieldChecked<FNumericProperty>(Property);
    Numeric->SetIntPropertyValue(OutValuePtr, Raw);
    return true;
  }
  if (FNumericProperty *Numeric = CastField<FNumericProperty>(Property)) {
    TSharedPtr<FJsonValue> Literal;
    if (!GetLiteral(Value, Literal, OutError))
      return false;
    const auto LiteralFailure = [&]() {
      if (Value->Type == EJson::Object)
        UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(OutError, TEXT("/literal"));
      return false;
    };
    const FString Kind = GetValueKindInternal(Property, bIgnoreFixedArray);
    if (Numeric->IsFloatingPoint()) {
      double Number = 0.0;
      if (!JsonNumber(Literal, Number, OutError) ||
          !ValidateNumericConstraints(Property, Number, OutError))
        return LiteralFailure();
      if (Kind == TEXT("float") && !ValidateFloat32Value(Number, OutError))
        return LiteralFailure();
      Numeric->SetFloatingPointPropertyValue(OutValuePtr, Number);
      return true;
    }
    if (Kind == TEXT("int64")) {
      if (Literal->Type != EJson::String)
        return ValueFailure(OutError, TEXT("int64_literal_must_be_decimal_string"), TEXT("A int64 literal must be a decimal string to preserve integer precision."), Literal, TEXT("decimal int64 string"), Value->Type == EJson::Object ? TEXT("/literal") : FString());
      int64 Number = 0;
      if (!UE::UAI::StateSemantic::TryParseInt64Decimal(Literal->AsString(), Number))
        return ValueFailure(OutError, TEXT("invalid_int64_literal"), TEXT("The literal is not a decimal integer within the int64 storage range."),
            Literal, TEXT("decimal int64 string"), Value->Type == EJson::Object ? TEXT("/literal") : FString());
      if (!ValidateSignedIntegerConstraints(Property, Number, OutError)) return LiteralFailure();
      Numeric->SetIntPropertyValue(OutValuePtr, Number);
      return true;
    }
    if (Kind == TEXT("uint64")) {
      if (Literal->Type != EJson::String)
        return ValueFailure(OutError, TEXT("uint64_literal_must_be_decimal_string"), TEXT("A uint64 literal must be a decimal string to preserve integer precision."), Literal, TEXT("decimal uint64 string"), Value->Type == EJson::Object ? TEXT("/literal") : FString());
      uint64 Number = 0;
      if (!UE::UAI::StateSemantic::TryParseUInt64Decimal(Literal->AsString(), Number))
        return ValueFailure(OutError, TEXT("invalid_uint64_literal"), TEXT("The literal is not a decimal integer within the uint64 storage range."),
            Literal, TEXT("decimal uint64 string"), Value->Type == EJson::Object ? TEXT("/literal") : FString());
      if (!ValidateUnsignedIntegerConstraints(Property, Number, OutError)) return LiteralFailure();
      Numeric->SetIntPropertyValue(OutValuePtr, Number);
      return true;
    }
    if (IsUnsignedNumericProperty(Numeric)) {
      uint64 Number = 0;
      if (!JsonNarrowUnsignedInteger(Kind, Literal, Number, OutError) ||
          !ValidateNumericConstraints(Property, static_cast<double>(Number),
                                      OutError))
        return LiteralFailure();
      Numeric->SetIntPropertyValue(OutValuePtr, Number);
      return true;
    }
    int64 Number = 0;
    if (!JsonNarrowSignedInteger(Kind, Literal, Number, OutError) ||
        !ValidateNumericConstraints(Property, static_cast<double>(Number),
                                    OutError))
      return LiteralFailure();
    Numeric->SetIntPropertyValue(OutValuePtr, Number);
    return true;
  }
  if (FStrProperty *String = CastField<FStrProperty>(Property)) {
    TSharedPtr<FJsonValue> Literal;
    if (!GetLiteral(Value, Literal, OutError)) return false;
    if (Literal->Type != EJson::String)
      return ValueFailure(OutError, TEXT("expected_string_literal"), TEXT("The string literal has the wrong JSON type."),
          Literal, TEXT("string"), Value->Type == EJson::Object ? TEXT("/literal") : FString());
    String->SetPropertyValue(OutValuePtr, Literal->AsString());
    return true;
  }
  if (FAnsiStrProperty *String = CastField<FAnsiStrProperty>(Property)) {
    TSharedPtr<FJsonValue> Literal;
    if (!GetLiteral(Value, Literal, OutError)) return false;
    if (Literal->Type != EJson::String)
      return ValueFailure(OutError, TEXT("expected_ansi_string_literal"), TEXT("The ansi string literal has the wrong JSON type."),
          Literal, TEXT("string"), Value->Type == EJson::Object ? TEXT("/literal") : FString());
    const auto Converted = StringCast<ANSICHAR>(*Literal->AsString());
    String->SetPropertyValue(
        OutValuePtr,
        FAnsiString::ConstructFromPtrSize(Converted.Get(), Converted.Length()));
    return true;
  }
  if (FUtf8StrProperty *String = CastField<FUtf8StrProperty>(Property)) {
    TSharedPtr<FJsonValue> Literal;
    if (!GetLiteral(Value, Literal, OutError)) return false;
    if (Literal->Type != EJson::String)
      return ValueFailure(OutError, TEXT("expected_utf8_string_literal"), TEXT("The utf8 string literal has the wrong JSON type."),
          Literal, TEXT("string"), Value->Type == EJson::Object ? TEXT("/literal") : FString());
    const auto Converted = StringCast<UTF8CHAR>(*Literal->AsString());
    String->SetPropertyValue(
        OutValuePtr,
        FUtf8String::ConstructFromPtrSize(Converted.Get(), Converted.Length()));
    return true;
  }
  if (FNameProperty *Name = CastField<FNameProperty>(Property)) {
    TSharedPtr<FJsonValue> Literal;
    if (!GetLiteral(Value, Literal, OutError)) return false;
    if (Literal->Type != EJson::String)
      return ValueFailure(OutError, TEXT("expected_name_literal"), TEXT("The name literal has the wrong JSON type."),
          Literal, TEXT("string"), Value->Type == EJson::Object ? TEXT("/literal") : FString());
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
      if (!OnlyFields(Object, {TEXT("literal")}, TEXT("expected_text_literal"), OutError)) return false;
      if (Literal->Type != EJson::String)
        return ValueFailure(OutError, TEXT("expected_text_literal"), TEXT("A text literal must be a string."), Literal, TEXT("string"), TEXT("/literal"));
      Text->SetPropertyValue(OutValuePtr,
                             FText::FromString(Literal->AsString()));
      return true;
    }
    if (!Object->TryGetObjectField(TEXT("localized"), Localized) ||
        !Localized || !Localized->IsValid() || Object->Values.Num() != 1 ||
        (*Localized)->Values.Num() != 3) {
      return ValueFailure(OutError, TEXT("expected_text_literal_or_localized"), TEXT("Text requires either a literal string or a localized object with source, namespace and key."),
          Object->TryGetField(TEXT("localized")), TEXT("localized object with exactly source, namespace and key"), TEXT("/localized"));
    }
    FString Source;
    FString Namespace;
    FString Key;
    if (!(*Localized)->TryGetStringField(TEXT("source"), Source) ||
        !(*Localized)->TryGetStringField(TEXT("namespace"), Namespace) ||
        !(*Localized)->TryGetStringField(TEXT("key"), Key)) {
      for (const TCHAR* Field : {TEXT("source"), TEXT("namespace"), TEXT("key")}) {
        const auto Actual = (*Localized)->TryGetField(Field);
        if (!Actual.IsValid() || Actual->Type != EJson::String)
          return ValueFailure(OutError, TEXT("invalid_localized_text"), TEXT("Each localized text member must be a string."), Actual, TEXT("string"), TEXT("/localized") + FieldPointer(Field));
      }
    }
    Text->SetPropertyValue(
        OutValuePtr,
        FText::ChangeKey(Namespace, Key, FText::FromString(Source)));
    return true;
  }
  if (FSoftObjectProperty *Soft = CastField<FSoftObjectProperty>(Property)) {
    TSharedPtr<FJsonValue> ReferenceValue = Value;
    if (Options.ScopedReferenceDecoder &&
        !Options.ScopedReferenceDecoder(Value, ReferenceValue, OutError)) return false;
    TSharedPtr<FJsonValue> PathValue;
    if (!GetReferencePath(ReferenceValue, PathValue, OutError))
      return false;
    if (PathValue->Type == EJson::Null)
      Soft->SetPropertyValue(OutValuePtr, FSoftObjectPtr());
    else if (PathValue->Type == EJson::String) {
      const FSoftObjectPath Path(PathValue->AsString());
      if (!IsCanonicalSoftReferencePath(Path))
        return ValueFailure(OutError, TEXT("invalid_soft_reference_path"), TEXT("The soft object reference is not a canonical asset path."), PathValue, TEXT("canonical soft object path"), ReferenceValue->Type == EJson::Object ? TEXT("/path") : FString());
      UObject *Referenced = Path.ResolveObject();
      if (Referenced && !Referenced->IsA(Soft->PropertyClass))
        return ValueFailure(OutError, TEXT("reference_wrong_class"), TEXT("The referenced object has a class that is not allowed by this property."),
            MakeShared<FJsonValueString>(Referenced->GetClass()->GetPathName()), Soft->PropertyClass->GetPathName(), ReferenceValue->Type == EJson::Object ? TEXT("/path") : FString());
      if (const FSoftClassProperty *SoftClass =
              CastField<FSoftClassProperty>(Property)) {
        const UClass *ReferencedClass = Cast<UClass>(Referenced);
        if (Referenced && (!ReferencedClass ||
                           (SoftClass->MetaClass &&
                            !ReferencedClass->IsChildOf(SoftClass->MetaClass))))
          return ValueFailure(OutError, TEXT("class_reference_not_child_of_allowed_class"), TEXT("The referenced class does not derive from the class allowed by this property."),
              MakeShared<FJsonValueString>(Referenced->GetPathName()), SoftClass->MetaClass ? SoftClass->MetaClass->GetPathName() : UClass::StaticClass()->GetPathName(),
              ReferenceValue->Type == EJson::Object ? TEXT("/path") : FString());
      }
      Soft->SetPropertyValue(OutValuePtr, FSoftObjectPtr(Path));
    } else
      return ValueFailure(OutError, TEXT("invalid_reference_path"), TEXT("A reference path must be a string or null."), PathValue, TEXT("string or null"), ReferenceValue->Type == EJson::Object ? TEXT("/path") : FString());
    return true;
  }
  if (FInterfaceProperty *Interface = CastField<FInterfaceProperty>(Property)) {
    UObject* Referenced = nullptr;
    if (!UeAgentPropertyValueCodec::DecodeObjectReference(Value, UObject::StaticClass(), nullptr,
        Referenced, OutError, Options.ScopedReferenceDecoder)) return false;
    FScriptInterface Result;
    if (Referenced) {
      if (!Referenced->GetClass()->ImplementsInterface(Interface->InterfaceClass))
        return ValueFailure(OutError, TEXT("invalid_interface_reference"), TEXT("The referenced object does not implement the required interface."),
            MakeShared<FJsonValueString>(Referenced->GetClass()->GetPathName()), Interface->InterfaceClass->GetPathName(), Value->Type == EJson::Object ? TEXT("/path") : FString());
      Result.SetObject(Referenced);
      Result.SetInterface(
          Referenced->GetInterfaceAddress(Interface->InterfaceClass));
    }
    Interface->SetPropertyValue(OutValuePtr, Result);
    return true;
  }
  if (FObjectPropertyBase *ObjectProperty =
          CastField<FObjectPropertyBase>(Property)) {
    UObject *Referenced = nullptr;
    const FClassProperty* ClassProperty = CastField<FClassProperty>(Property);
    if (!UeAgentPropertyValueCodec::DecodeObjectReference(Value, ObjectProperty->PropertyClass,
        ClassProperty ? ClassProperty->MetaClass : nullptr, Referenced, OutError, Options.ScopedReferenceDecoder)) return false;
    ObjectProperty->SetObjectPropertyValue(OutValuePtr, Referenced);
    return true;
  }
  if (FStructProperty *Struct = CastField<FStructProperty>(Property)) {
    const FString Kind = StructKind(Struct->Struct);
    if (Kind == TEXT("curve")) {
      TSharedPtr<FJsonObject> Object;
      if (!GetObject(Value, Object, OutError))
        return false;
      TArray<UE::UAI::Contract::FDiagnostic> Issues;
      if (!UeAgentCurveJson::TryApplyPropertyCurveJson(
              Property, OutValuePtr, Object, FString(), Issues) ||
          UeAgentCurveJson::HasError(Issues))
        return Fail(OutError, UeAgentAuthoringDiagnostics::MakeIssuesFailure(
            TEXT("invalid_curve_value"), Issues, TEXT("curve_decode")));
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
        !RequiredArray(Object, TEXT("items"), Items, TEXT("missing_array_items"), OutError))
      return false;
    if (!OnlyFields(Object, {TEXT("items")}, TEXT("unexpected_array_fields"), OutError)) return false;
    FScriptArrayHelper Helper(Array, OutValuePtr);
    Helper.EmptyAndAddValues(Items->Num());
    for (int32 Index = 0; Index < Items->Num(); ++Index)
      if (!DecodeInternal(Array->Inner, (*Items)[Index],
                          Helper.GetRawPtr(Index), Owner, Options, OutError,
                          Depth + 1, false)) {
        PrefixValueFailure(OutError, FString::Printf(TEXT("/items/%d"), Index), FString::Printf(TEXT("[%d]"), Index));
        return false;
      }
    return true;
  }
  if (FSetProperty *Set = CastField<FSetProperty>(Property)) {
    TSharedPtr<FJsonObject> Object;
    const TArray<TSharedPtr<FJsonValue>> *Items = nullptr;
    if (!GetObject(Value, Object, OutError) ||
        !RejectTruncatedObject(Object, OutError) ||
        !RequiredArray(Object, TEXT("items"), Items, TEXT("missing_set_items"), OutError))
      return false;
    if (!OnlyFields(Object, {TEXT("items")}, TEXT("unexpected_set_fields"), OutError)) return false;
    FScriptSetHelper Helper(Set, OutValuePtr);
    Helper.EmptyElements(Items->Num());
    for (int32 Index = 0; Index < Items->Num(); ++Index) {
      const int32 Internal = Helper.AddDefaultValue_Invalid_NeedsRehash();
      void *Element = Helper.GetElementPtr(Internal);
      if (!DecodeInternal(Set->ElementProp, (*Items)[Index], Element, Owner,
                          Options, OutError, Depth + 1, false)) {
        PrefixValueFailure(OutError, FString::Printf(TEXT("/items/%d"), Index), FString::Printf(TEXT("[%d]"), Index));
        return false;
      }
      for (int32 Existing = 0; Existing < Internal; ++Existing) {
        if (Helper.IsValidIndex(Existing) &&
            Set->ElementProp->Identical(Helper.GetElementPtr(Existing), Element,
                                        PPF_None)) {
          return ValueFailure(OutError, TEXT("duplicate_set_item"), FString::Printf(TEXT("The set item duplicates item %d."), Existing),
              (*Items)[Index], TEXT("unique set item"), FString::Printf(TEXT("/items/%d"), Index));
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
        !RequiredArray(Object, TEXT("entries"), Entries, TEXT("missing_map_entries"), OutError))
      return false;
    if (!OnlyFields(Object, {TEXT("entries")}, TEXT("unexpected_map_fields"), OutError)) return false;
    FScriptMapHelper Helper(Map, OutValuePtr);
    Helper.EmptyValues(Entries->Num());
    for (int32 Index = 0; Index < Entries->Num(); ++Index) {
      TSharedPtr<FJsonObject> Entry;
      const FString EntryPointer = FString::Printf(TEXT("/entries/%d"), Index);
      if (!GetObject((*Entries)[Index], Entry, OutError) ||
          !OnlyFields(Entry, {TEXT("key"), TEXT("value")}, TEXT("invalid_map_entry"), OutError)) {
        PrefixValueFailure(OutError, EntryPointer, FString::Printf(TEXT("[%d]"), Index));
        return false;
      }
      const TSharedPtr<FJsonValue> Key = Entry->TryGetField(TEXT("key"));
      const TSharedPtr<FJsonValue> Mapped = Entry->TryGetField(TEXT("value"));
      if (!Key.IsValid() || !Mapped.IsValid()) {
        return ValueFailure(OutError, TEXT("invalid_map_entry"), TEXT("Each map entry requires both key and value."),
            nullptr, TEXT("map key and value"), EntryPointer + (Key.IsValid() ? TEXT("/value") : TEXT("/key")));
      }
      const int32 Internal = Helper.AddDefaultValue_Invalid_NeedsRehash();
      void *KeyPtr = Helper.GetKeyPtr(Internal);
      if (!DecodeInternal(Map->KeyProp, Key, KeyPtr, Owner, Options, OutError,
                          Depth + 1, false)) {
        PrefixValueFailure(OutError, EntryPointer + TEXT("/key"), FString::Printf(TEXT("[%d].key"), Index));
        return false;
      }
      if (!DecodeInternal(Map->ValueProp, Mapped, Helper.GetValuePtr(Internal),
                          Owner, Options, OutError, Depth + 1, false)) {
        PrefixValueFailure(OutError, EntryPointer + TEXT("/value"), FString::Printf(TEXT("[%d].value"), Index));
        return false;
      }
      for (int32 Existing = 0; Existing < Internal; ++Existing) {
        if (Helper.IsValidIndex(Existing) &&
            Map->KeyProp->Identical(Helper.GetKeyPtr(Existing), KeyPtr,
                                    PPF_None)) {
          return ValueFailure(OutError, TEXT("duplicate_map_key"), FString::Printf(TEXT("The map key duplicates the key in entry %d."), Existing), Key,
              TEXT("unique map key"), EntryPointer + TEXT("/key"));
        }
      }
    }
    Helper.Rehash();
    return true;
  }
  if (FOptionalProperty *Optional = CastField<FOptionalProperty>(Property)) {
    TSharedPtr<FJsonObject> Object;
    bool bSet = false;
    if (!GetObject(Value, Object, OutError)) return false;
    if (!Object->TryGetBoolField(TEXT("is_set"), bSet))
      return ValueFailure(OutError, TEXT("missing_optional_is_set"), TEXT("An optional value requires a boolean is_set field."), Object->TryGetField(TEXT("is_set")), TEXT("boolean"), TEXT("/is_set"));
    if (!bSet) {
      if (!OnlyFields(Object, {TEXT("is_set")}, TEXT("unexpected_optional_fields"), OutError)) return false;
      Optional->MarkUnset(OutValuePtr);
      return true;
    }
    if (!OnlyFields(Object, {TEXT("is_set"), TEXT("value")}, TEXT("unexpected_optional_fields"), OutError)) return false;
    const TSharedPtr<FJsonValue> Inner = Object->TryGetField(TEXT("value"));
    if (!Inner.IsValid())
      return ValueFailure(OutError, TEXT("missing_optional_value"), TEXT("An optional value with is_set=true requires its value field."), Inner, TEXT("property value"), TEXT("/value"));
    const bool bDecoded = DecodeInternal(
        Optional->GetValueProperty(), Inner,
        Optional->MarkSetAndGetInitializedValuePointerToReplace(OutValuePtr),
        Owner, Options, OutError, Depth + 1, false);
    if (!bDecoded) PrefixValueFailure(OutError, TEXT("/value"), TEXT("value"));
    return bDecoded;
  }
  return PropertyFailure(OutError, TEXT("unsupported_property_type"), TEXT("The reflected property type has no authoring decoder."), Property);
}

static bool DecodeStructValueInternal(UScriptStruct *Struct,
                                      const FProperty *ConstraintsProperty,
                                      const TSharedPtr<FJsonValue> &Value,
                                      void *OutValuePtr, UObject *Owner,
                                      const FOptions &Options,
                                      FDiagnostic& OutError, int32 Depth) {
  if (!Struct || !OutValuePtr)
    return Fail(OutError, TEXT("invalid_struct_target"), TEXT("Struct decoding requires a reflected struct type and writable native value storage."));
  const FString Kind = StructKind(Struct);
  if (Kind == TEXT("instanced_struct")) {
    TSharedPtr<FJsonObject> Object;
    const TSharedPtr<FJsonObject> *Fields = nullptr;
    if (!GetObject(Value, Object, OutError) || !OnlyFields(Object, {TEXT("struct_path"), TEXT("fields")}, TEXT("invalid_instanced_struct_shape"), OutError)) return false;
    if (!Object->TryGetObjectField(TEXT("fields"), Fields) || !Fields ||
        !Fields->IsValid()) {
      return ValueFailure(OutError, TEXT("invalid_instanced_struct_shape"), TEXT("An instanced struct requires a fields object."), Object->TryGetField(TEXT("fields")), TEXT("object"), TEXT("/fields"));
    }
    const TSharedPtr<FJsonValue> StructPathValue =
        Object->TryGetField(TEXT("struct_path"));
    if (!StructPathValue.IsValid())
      return ValueFailure(OutError, TEXT("missing_instanced_struct_path"), TEXT("An instanced struct requires struct_path to select its type."), StructPathValue, TEXT("struct asset path or null"), TEXT("/struct_path"));
    FInstancedStruct &Instanced =
        *static_cast<FInstancedStruct *>(OutValuePtr);
    if (StructPathValue->Type == EJson::Null) {
      if (!(*Fields)->Values.IsEmpty())
        return ValueFailure(OutError, TEXT("unset_instanced_struct_has_fields"), TEXT("An unset instanced struct cannot contain member values."),
            MakeShared<FJsonValueNumber>((*Fields)->Values.Num()), TEXT("0 fields"), TEXT("/fields"));
      Instanced.Reset();
      return true;
    }
    if (StructPathValue->Type != EJson::String)
      return ValueFailure(OutError, TEXT("invalid_instanced_struct_path"), TEXT("The instanced struct type path must be a string or null."), StructPathValue, TEXT("string or null"), TEXT("/struct_path"));
    UScriptStruct *DynamicStruct =
        LoadObject<UScriptStruct>(nullptr, *StructPathValue->AsString());
    if (!DynamicStruct)
      return ValueFailure(OutError, TEXT("instanced_struct_type_not_found"), TEXT("The selected instanced struct type could not be loaded."), StructPathValue, TEXT("loadable UScriptStruct path"), TEXT("/struct_path"));
    FInstancedStruct Decoded;
    if (Instanced.GetScriptStruct() == DynamicStruct) Decoded = Instanced;
    else Decoded.InitializeAs(DynamicStruct);
    // Dynamic and ordinary structs share filtering, partial-value semantics
    // and native finalization. Preserve uncaptured state when the type stays.
    if (!DecodeReflectedStructFields(DynamicStruct, *Fields,
            Decoded.GetMutableMemory(), Owner, Options, OutError, Depth,
            TEXT("instanced_struct"))) return false;
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
    if (!GetObject(Value, Object, OutError) || !OnlyFields(Object, {TEXT("path")}, TEXT("invalid_soft_reference_shape"), OutError)) return false;
    const TSharedPtr<FJsonValue> PathValue =
        Object->TryGetField(TEXT("path"));
    if (!PathValue.IsValid())
      return ValueFailure(OutError, TEXT("missing_reference_path"), TEXT("The soft reference requires a path field."), PathValue, TEXT("string or null"), TEXT("/path"));
    FString Path;
    if (PathValue->Type == EJson::String) {
      Path = PathValue->AsString();
      if (!IsCanonicalSoftReferencePath(FSoftObjectPath(Path)))
        return ValueFailure(OutError, TEXT("invalid_soft_reference_path"), TEXT("The soft reference is not a canonical asset path."), PathValue, TEXT("canonical soft object path"), TEXT("/path"));
    } else if (PathValue->Type != EJson::Null)
      return ValueFailure(OutError, TEXT("invalid_reference_path"), TEXT("A reference path must be a string or null."), PathValue, TEXT("string or null"), TEXT("/path"));
    if (Kind == TEXT("soft_class_ref"))
      *static_cast<FSoftClassPath *>(OutValuePtr) = FSoftClassPath(Path);
    else
      *static_cast<FSoftObjectPath *>(OutValuePtr) = FSoftObjectPath(Path);
    return true;
  }
  if (Kind == TEXT("gameplay_tag")) {
    TSharedPtr<FJsonValue> Literal;
    if (!GetLiteral(Value, Literal, OutError)) return false;
    if (Literal->Type != EJson::String)
      return ValueFailure(OutError, TEXT("expected_gameplay_tag_literal"), TEXT("The gameplay tag literal has the wrong JSON type."),
          Literal, TEXT("string"), Value->Type == EJson::Object ? TEXT("/literal") : FString());
    const FGameplayTag Tag = UGameplayTagsManager::Get().RequestGameplayTag(
        FName(*Literal->AsString()), false);
    if (!Tag.IsValid() && !Literal->AsString().IsEmpty())
      return ValueFailure(OutError, TEXT("unknown_gameplay_tag"), TEXT("The gameplay tag is not registered."), Literal, TEXT("registered gameplay tag or empty string"),
          Value->Type == EJson::Object ? TEXT("/literal") : FString());
    *static_cast<FGameplayTag *>(OutValuePtr) = Tag;
    return true;
  }
  if (Kind == TEXT("gameplay_tags")) {
    TSharedPtr<FJsonObject> Object;
    const TArray<TSharedPtr<FJsonValue>> *Items = nullptr;
    if (!GetObject(Value, Object, OutError) ||
        !OnlyFields(Object, {TEXT("items")}, TEXT("invalid_gameplay_tags_shape"), OutError) ||
        !RequiredArray(Object, TEXT("items"), Items, TEXT("invalid_gameplay_tags_shape"), OutError)) return false;
    FGameplayTagContainer Result;
    for (int32 Index = 0; Index < Items->Num(); ++Index) {
      const auto& Item = (*Items)[Index];
      const FString Pointer = FString::Printf(TEXT("/items/%d"), Index);
      if (!Item.IsValid() || Item->Type != EJson::String)
        return ValueFailure(OutError, TEXT("invalid_gameplay_tag_item"), TEXT("Each gameplay tag must be a string."), Item, TEXT("string"), Pointer);
      const FGameplayTag Tag = UGameplayTagsManager::Get().RequestGameplayTag(
          FName(*Item->AsString()), false);
      if (!Tag.IsValid())
        return ValueFailure(OutError, TEXT("unknown_gameplay_tag"), TEXT("The gameplay tag is not registered."), Item, TEXT("registered gameplay tag"), Pointer);
      if (Result.HasTagExact(Tag))
        return ValueFailure(OutError, TEXT("duplicate_gameplay_tag"), TEXT("The gameplay tag duplicates an earlier item."), Item, TEXT("unique gameplay tag"), Pointer);
      Result.AddTag(Tag);
    }
    *static_cast<FGameplayTagContainer *>(OutValuePtr) = MoveTemp(Result);
    return true;
  }
  bool bHandled = false;
  if (!DecodeKnownStructValue(Struct, ConstraintsProperty, Value, OutValuePtr,
                              OutError, bHandled))
    return false;
  if (bHandled)
    return true;

  TSharedPtr<FJsonObject> Object;
  const TSharedPtr<FJsonObject> *Fields = nullptr;
  if (!GetObject(Value, Object, OutError)) return false;
  if (!Object->TryGetObjectField(TEXT("fields"), Fields) || !Fields ||
      !Fields->IsValid()) {
    return ValueFailure(OutError, TEXT("missing_struct_fields"), TEXT("A reflected struct value requires a fields object."), Object->TryGetField(TEXT("fields")), TEXT("object"), TEXT("/fields"));
  }
  for (const auto& Field : Object->Values) {
    if (Field.Key == TEXT("fields")) continue;
    auto Error = FDiagnostic::Failure(TEXT("unexpected_struct_value_field"),
        TEXT("A reflected struct value accepts only the fields object."), TEXT("property_value_decode"));
    Error.ActualValue = MakeShared<FJsonValueString>(Field.Key);
    Error.Source.InternalJsonPath = TEXT("/") + Field.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
    return Fail(OutError, MoveTemp(Error));
  }

  return DecodeReflectedStructFields(Struct, *Fields, OutValuePtr, Owner, Options, OutError, Depth, TEXT("struct"));
}

static bool DecodeReflectedStructFields(UScriptStruct *Struct,
                                       const TSharedPtr<FJsonObject>& Fields,
                                       void *OutValuePtr, UObject *Owner,
                                       const FOptions &Options,
                                       FDiagnostic& OutError, int32 Depth,
                                       const TCHAR* Shape) {
  TOptional<FEnvTraceData> TraceBefore;
  if (UeAgentEnvTraceDataValue::IsType(Struct)) {
    TraceBefore = UeAgentEnvTraceDataValue::Canonical(*static_cast<const FEnvTraceData*>(OutValuePtr));
    UeAgentEnvTraceDataValue::CopyChannels(TraceBefore.GetValue(), *static_cast<FEnvTraceData*>(OutValuePtr));
  }
  TSet<FString> Expected;
  for (TFieldIterator<FProperty> It(Struct, EFieldIterationFlags::IncludeSuper);
       It; ++It) {
    FProperty *Field = *It;
    if (Field->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
      continue;
    if (Options.StructFieldFilter && !Options.StructFieldFilter(Struct, Field)) {
      if (Fields->HasField(Field->GetName()))
        return ValueFailure(OutError, TEXT("filtered_struct_field_") + Field->GetName(), TEXT("This struct member is excluded from the current authoring surface."),
            MakeShared<FJsonValueString>(Field->GetName()), TEXT("authorable struct member"), TEXT("/fields") + FieldPointer(Field->GetName()));
      continue;
    }
    if (!IsSupportedValue(Field, Field->ContainerPtrToValuePtr<void>(OutValuePtr),
            OutError, nullptr, &Options.StructFieldFilter)) {
      PrefixValueFailure(OutError, TEXT("/fields/") + Field->GetName().Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")), Field->GetName());
      return false;
    }
    Expected.Add(Field->GetName());
    const TSharedPtr<FJsonValue> FieldValue =
        Fields->TryGetField(Field->GetName());
    if (!FieldValue.IsValid()) {
      if (Options.bAllowPartialStructFields)
        continue;
      return ValueFailure(OutError, FString::Printf(TEXT("missing_%s_field_%s"), Shape, *Field->GetName()), TEXT("A required struct member value is missing."),
          FieldValue, Field->GetCPPType(), TEXT("/fields") + FieldPointer(Field->GetName()));
    }
    if (!DecodeInternal(Field, FieldValue,
                        Field->ContainerPtrToValuePtr<void>(OutValuePtr), Owner,
                        Options, OutError, Depth + 1, false)) {
      PrefixValueFailure(OutError, TEXT("/fields/") + Field->GetName().Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")), Field->GetName());
      return false;
    }
  }
  for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair : Fields->Values) {
    if (!Expected.Contains(Pair.Key))
      return ValueFailure(OutError, FString::Printf(TEXT("unknown_%s_field_%s"), Shape, *Pair.Key), TEXT("The field is not a supported member of this struct."),
          MakeShared<FJsonValueString>(Pair.Key), Struct->GetPathName(), TEXT("/fields") + FieldPointer(Pair.Key));
  }
  // Reflected field assignment must finalize native derived state just as
  // Blueprint struct construction does (for example FKey's cached details).
  if (Struct->StructFlags & STRUCT_PostScriptConstruct) {
    if (auto* Ops = Struct->GetCppStructOps())
      Ops->PostScriptConstruct(OutValuePtr);
  }
  if (TraceBefore.IsSet())
    return UeAgentEnvTraceDataValue::ReconcileEdit(*static_cast<FEnvTraceData*>(OutValuePtr),
        TraceBefore.GetValue(), Fields->HasField(TEXT("TraceChannel")),
        Fields->HasField(TEXT("SerializedChannel")), OutError);
  return true;
}

FString EncodeEnumValueName(const UEnum* Enum, int64 Raw) {
  return Enum ? Enum->GetNameStringByValue(Raw) : FString();
}

bool IsUnchangedEmptyOwnedObjectArray(const FProperty* Property, const void* ValuePtr,
    const TSharedPtr<FJsonValue>& PreviousValue, const TSharedPtr<FJsonValue>& PreviousDefinition) {
  const auto* Array = CastField<FArrayProperty>(Property);
  if (!Array || !ValuePtr || Array->ArrayDim != 1 || !CastField<FObjectPropertyBase>(Array->Inner)
      || !UeAgentAuthoringReflection::IsOwnedObjectProperty(Array->Inner)
      || FScriptArrayHelper(Array, ValuePtr).Num() != 0) return false;
  const auto EmptyItems = [](const TSharedPtr<FJsonValue>& Value, bool bDefinition) {
    if (!Value || Value->Type != EJson::Object || !Value->AsObject()) return false;
    const auto& Fields = Value->AsObject()->Values;
    if (Fields.Num() != (bDefinition ? 2 : 1)) return false;
    bool bItems = false, bKind = !bDefinition;
    for (const auto& Field : Fields) {
      if (Field.Key.Equals(TEXT("items"), ESearchCase::CaseSensitive))
        bItems = Field.Value && Field.Value->Type == EJson::Array && Field.Value->AsArray().IsEmpty();
      else if (bDefinition && Field.Key.Equals(TEXT("element_kind"), ESearchCase::CaseSensitive))
        bKind = Field.Value && Field.Value->Type == EJson::String
            && Field.Value->AsString().Equals(TEXT("owned_object"), ESearchCase::CaseSensitive);
      else return false;
    }
    return bItems && bKind;
  };
  // This is precisely EncodeInternal's empty object-array result. No element,
  // default struct template or reference callback is visited for this shape.
  return EmptyItems(PreviousValue, false) && EmptyItems(PreviousDefinition, true);
}

bool IsUnchangedBuiltinScalar(const FProperty* Property, const void* ValuePtr,
    const TSharedPtr<FJsonValue>& PreviousValue, const TSharedPtr<FJsonValue>& PreviousDefinition) {
  if (!Property || !ValuePtr || Property->ArrayDim != 1 || PreviousDefinition
      || GetPropertyEnum(Property) || IsBitmaskProperty(Property)
      || !PreviousValue || PreviousValue->Type != EJson::Object || !PreviousValue->AsObject()) return false;
  const auto& Fields = PreviousValue->AsObject()->Values;
  if (Fields.Num() != 1) return false;
  const auto Field = Fields.CreateConstIterator();
  if (!Field.Key().Equals(TEXT("literal"), ESearchCase::CaseSensitive) || !Field.Value()) return false;
  const auto& Literal = Field.Value();
  const auto SameString = [&Literal](const FString& Text) {
    return Literal->Type == EJson::String && Literal->AsString().Equals(Text, ESearchCase::CaseSensitive);
  };
  if (const auto* Bool = CastField<FBoolProperty>(Property))
    return Literal->Type == EJson::Boolean && Literal->AsBool() == Bool->GetPropertyValue(ValuePtr);
  if (const auto* Numeric = CastField<FNumericProperty>(Property)) {
    const FString Kind = GetValueKindInternal(Property, false);
    if (Kind == TEXT("int64")) return SameString(LexToString(Numeric->GetSignedIntPropertyValue(ValuePtr)));
    if (Kind == TEXT("uint64")) return SameString(LexToString(Numeric->GetUnsignedIntPropertyValue(ValuePtr)));
    if (Literal->Type != EJson::Number) return false;
    const double Number = Numeric->IsFloatingPoint() ? CanonicalNumber(Numeric->GetFloatingPointPropertyValue(ValuePtr))
        : IsUnsignedNumericProperty(Numeric) ? static_cast<double>(Numeric->GetUnsignedIntPropertyValue(ValuePtr))
        : static_cast<double>(Numeric->GetSignedIntPropertyValue(ValuePtr));
    if (!FMath::IsFinite(Number)) return false;
    // Match canonical decimal text, including NumberString inputs, without
    // collapsing int64, negative zero or a near-but-different decimal via ==.
    const auto Current = NativeNumericValue(Numeric, Number);
    FString BeforeLexeme, NowLexeme, BeforeNormalized, NowNormalized;
    return UE::UAI::StateSemantic::CanonicalJson::TryGetNumberLexeme(*Literal, BeforeLexeme)
        && UE::UAI::StateSemantic::CanonicalJson::TryGetNumberLexeme(*Current, NowLexeme)
        && UE::UAI::StateSemantic::TryNormalizeNumberLexeme(BeforeLexeme, BeforeNormalized)
        && UE::UAI::StateSemantic::TryNormalizeNumberLexeme(NowLexeme, NowNormalized)
        && BeforeNormalized.Equals(NowNormalized, ESearchCase::CaseSensitive);
  }
  if (const auto* String = CastField<FStrProperty>(Property)) return SameString(String->GetPropertyValue(ValuePtr));
  if (const auto* Name = CastField<FNameProperty>(Property)) return SameString(Name->GetPropertyValue(ValuePtr).ToString());
  // FText, enum and all recursive/reference encoders retain their full path.
  return false;
}

bool TryEncodePathOnlyAssetReferences(FProperty* Property, const void* ValuePtr,
    UObject* Owner, TSharedPtr<FJsonValue>& OutValue) {
  OutValue.Reset();
  TFunction<bool(const FProperty*)> IsPathShape = [&](const FProperty* Field) {
    if (!Field || Field->HasAnyPropertyFlags(CPF_InstancedReference | CPF_ContainsInstancedReference)) return false;
    if (const auto* Array = CastField<FArrayProperty>(Field)) return IsPathShape(Array->Inner);
    const auto* Object = CastField<FObjectPropertyBase>(Field);
    return Object && !UeAgentAuthoringReflection::IsOwnedObjectProperty(Object);
  };
  if (!ValuePtr || !IsPathShape(Property)) return false;
  TFunction<bool(const FProperty*, const void*, bool)> HasPathValue =
      [&](const FProperty* Field, const void* Data, bool bSingleElement) {
    if (!bSingleElement && Field->ArrayDim > 1) {
      for (int32 Index = 0; Index < Field->ArrayDim; ++Index)
        if (!HasPathValue(Field, static_cast<const uint8*>(Data) + Index * Field->ElementSize, true)) return false;
      return true;
    }
    if (const auto* Array = CastField<FArrayProperty>(Field)) {
      FScriptArrayHelper Values(Array, Data);
      for (int32 Index = 0; Index < Values.Num(); ++Index)
        if (!HasPathValue(Array->Inner, Values.GetRawPtr(Index), false)) return false;
      return true;
    }
    if (CastField<FSoftObjectProperty>(Field)) return true;
    const auto* Object = CastField<FObjectPropertyBase>(Field);
    if (!Object || UeAgentAuthoringReflection::IsExistingInlineObjectProperty(Object, Data, Owner)) return false;
    const UObject* Referenced = Object->GetObjectPropertyValue(Data);
    return !Referenced || Referenced->IsAsset();
  };
  if (!HasPathValue(Property, ValuePtr, false)) return false;
  OutValue = Encode(Property, ValuePtr, Owner, FOptions{});
  return OutValue.IsValid();
}

TSharedPtr<FJsonValue> EncodeReferencePath(const FString& Path) {
  const auto Object = MakeShared<FJsonObject>();
  if (Path.IsEmpty()) Object->SetField(TEXT("path"), MakeShared<FJsonValueNull>());
  else Object->SetStringField(TEXT("path"), Path);
  return MakeObjectValue(Object);
}

bool DecodeReferencePath(const TSharedPtr<FJsonValue>& Value, TOptional<FString>& OutPath, FDiagnostic& OutError) {
  TSharedPtr<FJsonValue> Path;
  if (!GetReferencePath(Value, Path, OutError)) return false;
  if (Path->Type == EJson::Null) OutPath.Reset();
  else if (Path->Type == EJson::String) OutPath = Path->AsString();
  else return ValueFailure(OutError, TEXT("invalid_reference_path"), TEXT("A reference path must be a string or null."), Path, TEXT("string or null"),
      Value->Type == EJson::Object ? TEXT("/path") : FString());
  return true;
}

bool DecodeObjectReference(const TSharedPtr<FJsonValue>& Value, UClass* PropertyClass,
                          UClass* MetaClass, UObject*& OutObject, FDiagnostic& OutError,
                          const FScopedReferenceDecoder& Resolver) {
  if (!PropertyClass) return Fail(OutError, TEXT("reference_class_context_missing"), TEXT("Object reference decoding requires the allowed property class."));
  UObject* Referenced = nullptr;
  TSharedPtr<FJsonValue> ReferenceValue = Value;
  if (Resolver && !Resolver(Value, ReferenceValue, OutError)) return false;
  TOptional<FString> Path;
  if (!DecodeReferencePath(ReferenceValue, Path, OutError)) return false;
  if (Path.IsSet()) {
    Referenced = FindObject<UObject>(nullptr, *Path.GetValue());
    if (!Referenced) Referenced = StaticLoadObject(PropertyClass, nullptr, *Path.GetValue());
    Referenced = ResolveLoadedRedirector(Referenced);
    if (!Referenced) return ValueFailure(OutError, TEXT("reference_not_found_or_wrong_class"), TEXT("The reference could not be resolved to an object of the required class."),
        MakeShared<FJsonValueString>(Path.GetValue()), PropertyClass->GetPathName(), ReferenceValue->Type == EJson::Object ? TEXT("/path") : FString());
  }
  if (Referenced && !Referenced->IsA(PropertyClass)) {
    ValueFailure(OutError, TEXT("reference_not_found_or_wrong_class"), TEXT("The referenced object has a class that is not allowed by this property."),
        MakeShared<FJsonValueString>(Referenced->GetClass()->GetPathName()), PropertyClass->GetPathName(), ReferenceValue->Type == EJson::Object ? TEXT("/path") : FString());
    OutError.TargetPath = Referenced->GetPathName();
    return false;
  }
  const UClass* ReferencedClass = Cast<UClass>(Referenced);
  if (ReferencedClass && MetaClass && !ReferencedClass->IsChildOf(MetaClass))
    return ValueFailure(OutError, TEXT("class_reference_not_child_of_allowed_class"), TEXT("The referenced class does not derive from the allowed class."),
        MakeShared<FJsonValueString>(ReferencedClass->GetPathName()), MetaClass->GetPathName(), ReferenceValue->Type == EJson::Object ? TEXT("/path") : FString());
  OutObject = Referenced;
  return true;
}

bool DecodeEnumValue(const UEnum* Enum, const TSharedPtr<FJsonValue>& Value, int64& OutRaw, FDiagnostic& OutError) {
  if (!Enum) return Fail(OutError, TEXT("missing_enum_context"), TEXT("Enum decoding requires a reflected enum type."));
    FString Selected;
    if (Value.IsValid() && Value->Type == EJson::String) {
      Selected = Value->AsString();
    } else {
      TSharedPtr<FJsonObject> Object;
      if (!GetObject(Value, Object, OutError)) return false;
      if (!Object->TryGetStringField(TEXT("selected"), Selected))
        return ValueFailure(OutError, TEXT("missing_enum_selected"), TEXT("The enum value requires a selected name string."), Object->TryGetField(TEXT("selected")), TEXT("enum name string"), TEXT("/selected"));
      if (!OnlyFields(Object, {TEXT("selected")}, TEXT("unexpected_enum_fields"), OutError)) return false;
    }
    if (!UeAgentAuthoringTypeDescriptors::TryResolveEnumValueName(Enum, Selected, OutRaw))
      return ValueFailure(OutError, TEXT("unknown_enum_value"), TEXT("The selected value is not a member of the enum."),
          MakeShared<FJsonValueString>(Selected), Enum->GetPathName(), Value->Type == EJson::Object ? TEXT("/selected") : FString());
  return true;
}

bool Decode(FProperty *Property, const TSharedPtr<FJsonValue> &Value,
            void *OutValuePtr, UObject *Owner, const FOptions &Options,
            FDiagnostic& OutError, int32 Depth, bool bSingleFixedElement) {
  return DecodeInternal(Property, Value, OutValuePtr, Owner, Options, OutError,
                        Depth, bSingleFixedElement);
}

bool TryConvertFloat32(double Number, float& OutValue, FDiagnostic& OutError) {
  if (!ValidateFloat32Value(Number, OutError)) return false;
  OutValue = static_cast<float>(Number);
  return true;
}

bool TryConvertInt32(double Number, int32& OutValue, FDiagnostic& OutError, bool bRoundToNearest) {
  if (!FMath::IsFinite(Number)) return ValueFailure(OutError, TEXT("int32_non_finite_value"), TEXT("A non-finite number cannot be converted to int32."), NumberFact(Number), TEXT("finite number"));
  const double NativeValue = bRoundToNearest ? FMath::FloorToDouble(Number + 0.5) : FMath::TruncToDouble(Number);
  if (NativeValue < MIN_int32 || NativeValue > MAX_int32) {
    return ValueFailure(OutError, TEXT("int32_value_out_of_range"), TEXT("The converted integer is outside the int32 storage range."), NumberFact(Number),
        FString::Printf(TEXT("%d..%d after %s"), MIN_int32, MAX_int32, bRoundToNearest ? TEXT("rounding") : TEXT("truncation")));
  }
  OutValue = static_cast<int32>(NativeValue);
  return true;
}

bool TryNormalizeQuaternion(FQuat& Value, FDiagnostic& OutError) {
  return NormalizeQuaternionValue(Value, OutError);
}

bool AreRotationsEquivalent(const FQuat& Left, const FQuat& Right, FDiagnostic& Error) {
  FQuat NormalizedLeft = Left;
  FQuat NormalizedRight = Right;
  if (!NormalizeQuaternionValue(NormalizedLeft, Error) ||
      !NormalizeQuaternionValue(NormalizedRight, Error)) return false;
  // A dot-product error of 1e-4 admits about 1.62 degrees of rotation. Compare
  // the angle instead, retaining winding and quaternion-sign equivalence.
  const double AngleDegrees = FMath::RadiansToDegrees(
      NormalizedLeft.AngularDistance(NormalizedRight));
  return AngleDegrees <= static_cast<double>(KINDA_SMALL_NUMBER);
}

TSharedRef<FJsonObject> MakeTransformVectorComparisonDefinition() {
  const auto Definition = MakeShared<FJsonObject>();
  Definition->SetStringField(TEXT("comparison_semantics"), TransformVectorComparison);
  return Definition;
}

TOptional<FValueComparison> CompareCanonicalValues(const TSharedPtr<FJsonObject>& Type,
    const TSharedPtr<FJsonObject>& Definition, const TSharedPtr<FJsonValue>& Left,
    const TSharedPtr<FJsonValue>& Right, bool bStoredValues) {
  FDiagnostic Error;
  const auto Invalid = [&](const TCHAR* Reason) {
    if (Error.IsEmpty()) {
      Error = FDiagnostic::Failure(TEXT("canonical_comparison_input_invalid"), Reason, TEXT("semantic_compare"));
      Error.SourceKind = TEXT("comparison_contract");
      Error.ExpectedValue = MakeShared<FJsonValueObject>(Type);
      const auto Inputs = MakeShared<FJsonObject>();
      if (Left) Inputs->SetField(TEXT("expected_input"), Left);
      if (Right) Inputs->SetField(TEXT("actual_input"), Right);
      Error.ActualValue = MakeShared<FJsonValueObject>(Inputs);
    }
    return FValueComparison::Failed(Error);
  };
  const auto Decide = [&](bool bEqual) {
    if (!Error.IsEmpty()) return FValueComparison::Failed(Error);
    return bEqual ? FValueComparison() : FValueComparison::Different(FString(), Left, Right);
  };
  if (!Type || !Left || !Right) return {};
  FString Kind;
  if (!Type->TryGetStringField(TEXT("kind"), Kind)) return {};
  FString Encoding;
  if (Definition && Definition->TryGetStringField(TEXT("value_encoding"), Encoding) && Encoding == TEXT("lexeme")) {
    // Some existing public carriers store a scalar as text. Decode only that
    // declared carrier, then reuse the ordinary typed codec rules unchanged.
    const auto DecodeLexeme = [&](const TSharedPtr<FJsonValue>& Value) -> TSharedPtr<FJsonValue> {
      if (!Value || Value->Type != EJson::String) return Value;
      if (Kind == TEXT("bool")) {
        if (Value->AsString() == TEXT("true")) return MakeShared<FJsonValueBoolean>(true);
        if (Value->AsString() == TEXT("false")) return MakeShared<FJsonValueBoolean>(false);
        return nullptr;
      }
      if (Kind == TEXT("float") || Kind == TEXT("double") || Kind == TEXT("int")
          || Kind == TEXT("int8") || Kind == TEXT("int16") || Kind == TEXT("int32")
          || Kind == TEXT("uint8") || Kind == TEXT("uint16") || Kind == TEXT("uint32"))
        return MakeShared<FJsonValueNumberString>(Value->AsString());
      return Value;
    };
    const auto DecodedLeft = DecodeLexeme(Left), DecodedRight = DecodeLexeme(Right);
    if (!DecodedLeft || !DecodedRight) return Invalid(TEXT("A declared boolean lexeme must be 'true' or 'false'."));
    const auto NativeDefinition = MakeShared<FJsonObject>(); NativeDefinition->Values = Definition->Values;
    NativeDefinition->RemoveField(TEXT("value_encoding"));
    return CompareCanonicalValues(Type, NativeDefinition, DecodedLeft, DecodedRight, bStoredValues);
  }
  TSharedPtr<FJsonValue> A, B;
  auto Literals = [&] { return GetLiteral(Left, A, Error) && GetLiteral(Right, B, Error); };
  if (Kind == TEXT("float") || Kind == TEXT("double")) {
    double X, Y;
    if (!Literals() || !JsonNumber(A, X, Error) || !JsonNumber(B, Y, Error)) return Decide(false);
    if (Kind == TEXT("double")) return Decide(X == Y);
    float FX, FY;
    return Decide(TryConvertFloat32(X, FX, Error) && TryConvertFloat32(Y, FY, Error) && FX == FY);
  }
  if (Kind == TEXT("int64") || Kind == TEXT("uint64")) {
    if (!Literals() || A->Type != EJson::String || B->Type != EJson::String)
      return Invalid(TEXT("A 64-bit integer comparison requires decimal string literals."));
    if (Kind == TEXT("int64")) {
      int64 X, Y;
      if (!UE::UAI::StateSemantic::TryParseInt64Decimal(A->AsString(), X)
          || !UE::UAI::StateSemantic::TryParseInt64Decimal(B->AsString(), Y))
        return Invalid(TEXT("A comparison input is not a valid signed 64-bit decimal integer."));
      return Decide(X == Y);
    }
    uint64 X, Y;
    if (!UE::UAI::StateSemantic::TryParseUInt64Decimal(A->AsString(), X)
        || !UE::UAI::StateSemantic::TryParseUInt64Decimal(B->AsString(), Y))
      return Invalid(TEXT("A comparison input is not a valid unsigned 64-bit decimal integer."));
    return Decide(X == Y);
  }
  if (Kind == TEXT("int8") || Kind == TEXT("int16") || Kind == TEXT("int32") || Kind == TEXT("int")) {
    int64 X, Y;
    return Decide(Literals() && JsonNarrowSignedInteger(Kind, A, X, Error)
        && JsonNarrowSignedInteger(Kind, B, Y, Error) && X == Y);
  }
  if (Kind == TEXT("uint8") || Kind == TEXT("uint16") || Kind == TEXT("uint32")) {
    uint64 X, Y;
    return Decide(Literals() && JsonNarrowUnsignedInteger(Kind, A, X, Error)
        && JsonNarrowUnsignedInteger(Kind, B, Y, Error) && X == Y);
  }
  if (Kind == TEXT("name") || Kind == TEXT("gameplay_tag")) {
    if (!Literals() || A->Type != EJson::String || B->Type != EJson::String)
      return Invalid(TEXT("A name or gameplay tag comparison requires string literals."));
    return Decide(FName(*A->AsString()) == FName(*B->AsString()));
  }
  if (Kind == TEXT("bool") || Kind == TEXT("string") || Kind == TEXT("text")) {
    // Localized text is a record: namespace, key and source all remain significant.
    if (!Literals()) {
      if (Kind == TEXT("text")) return {};
      return Invalid(TEXT("The comparison input cannot be decoded as the declared literal."));
    }
    const auto Expected = Kind == TEXT("bool") ? EJson::Boolean : EJson::String;
    if (A->Type != Expected || B->Type != Expected)
      return Invalid(TEXT("A literal comparison input has the wrong JSON type for its declaration."));
    return Decide(UE::UAI::StateSemantic::CanonicalJson::AreJsonValuesEquivalent(A, B));
  }
  if (Kind == TEXT("enum")) {
    FString Path;
    if (!Type->TryGetStringField(TEXT("enum_path"), Path)) return {};
    const UEnum* Enum = FindObject<UEnum>(nullptr, *Path);
    if (!Enum) return Invalid(TEXT("The declared comparison enum could not be resolved."));
    int64 X, Y;
    return Decide(DecodeEnumValue(Enum, Left, X, Error) && DecodeEnumValue(Enum, Right, Y, Error) && X == Y);
  }
  if (Kind == TEXT("bitmask")) {
    const TSharedPtr<FJsonObject>* Masks = nullptr;
    const TSharedPtr<FJsonObject>* Contract = nullptr;
    if (!Definition || !Definition->TryGetObjectField(TEXT("contract"), Contract)
        || !(*Contract)->TryGetObjectField(TEXT("bitmask_masks"), Masks)) return {};
    FString Path;
    Type->TryGetStringField(TEXT("enum_path"), Path);
    const UEnum* Enum = FindObject<UEnum>(nullptr, *Path);
    if (!Enum) return Invalid(TEXT("The declared comparison bitmask enum could not be resolved."));
    FString StorageText;
    uint64 StorageMask;
    if (!(*Contract)->TryGetStringField(TEXT("storage_mask"), StorageText)
        || !UE::UAI::StateSemantic::TryParseUInt64Decimal(StorageText, StorageMask)) return {};
    auto ResolveMask = [&](int64 Raw, uint64& Mask) {
      FString Bits;
      return (*Masks)->TryGetStringField(EncodeEnumValueName(Enum, Raw), Bits)
          && UE::UAI::StateSemantic::TryParseUInt64Decimal(Bits, Mask);
    };
    uint64 X, Y;
    return Decide(DecodeBitmaskValue(Enum, Left, StorageMask, ResolveMask, X, Error)
        && DecodeBitmaskValue(Enum, Right, StorageMask, ResolveMask, Y, Error) && X == Y);
  }
  if (Kind == TEXT("object_ref") || Kind == TEXT("class_ref") || Kind == TEXT("soft_object_ref")
      || Kind == TEXT("soft_class_ref") || Kind == TEXT("weak_object_ref")
      || Kind == TEXT("lazy_object_ref") || Kind == TEXT("interface_ref")) {
    TOptional<FString> X, Y;
    // Inline owned objects and scoped references retain their existing recursive/identity owner.
    if (!DecodeReferencePath(Left, X, Error) || !DecodeReferencePath(Right, Y, Error)) return {};
    if (X.IsSet() != Y.IsSet()) return Decide(false);
    return Decide(!X.IsSet() || FSoftObjectPath(X.GetValue()) == FSoftObjectPath(Y.GetValue()));
  }
  FString StructPath;
  if (!Type->TryGetStringField(TEXT("struct_path"), StructPath)) return {};
  UScriptStruct* Struct = FindObject<UScriptStruct>(nullptr, *StructPath);
  if (!bStoredValues && Struct == FCollisionResponse::StaticStruct() && Kind == TEXT("struct")) {
    FStructOnScope X(Struct), Y(Struct);
    if (!DecodeStructValue(Struct, Left, X.GetStructMemory(), Error)
        || !DecodeStructValue(Struct, Right, Y.GetStructMemory(), Error)) return Decide(false);
    return CompareEffectiveStruct(Struct, X.GetStructMemory(), Y.GetStructMemory(), {});
  }
  if (Kind == TEXT("struct") || Kind == TEXT("instanced_struct") || Kind == TEXT("property_bag")
      || Kind == TEXT("curve") || Kind == TEXT("gameplay_tags")) return {};
  if (!Struct || UeAgentAuthoringTypeDescriptors::GetStructKind(Struct) != Kind)
    return Invalid(TEXT("The comparison struct could not be resolved with the declared value kind."));
  if (bStoredValues && Kind == TEXT("transform")) {
    TSharedPtr<FJsonObject> X, Y;
    if (!GetObject(Left, X, Error) || !GetObject(Right, Y, Error)
        || X->Values.Num() != 3 || Y->Values.Num() != 3)
      return Invalid(TEXT("A stored transform comparison requires translation, rotation and scale."));
    const bool bFloat = Struct == TVariantStructure<FTransform3f>::Get();
    for (const TCHAR* Field : {TEXT("translation"), TEXT("rotation"), TEXT("scale")}) {
      const bool bRotation = FCString::Strcmp(Field, TEXT("rotation")) == 0;
      const auto* ComponentStruct = bRotation
          ? (bFloat ? TVariantStructure<FRotator3f>::Get() : TBaseStructure<FRotator>::Get())
          : (bFloat ? TVariantStructure<FVector3f>::Get() : TBaseStructure<FVector>::Get());
      if (!X->HasField(Field) || !Y->HasField(Field))
        return Invalid(TEXT("A stored transform comparison is missing a required component.")).WithPrefix(FString(TEXT("/")) + Field);
      const auto Equal = CompareCanonicalValues(UeAgentAuthoringTypeDescriptors::DescribeStruct(ComponentStruct).ToJsonObject(),
          nullptr, X->TryGetField(Field), Y->TryGetField(Field), true);
      if (!Equal.IsSet()) return Invalid(TEXT("The declared transform component has no typed comparator.")).WithPrefix(FString(TEXT("/")) + Field);
      if (!Equal->bEquivalent) return Equal->WithPrefix(FString(TEXT("/")) + Field);
    }
    return FValueComparison();
  }
  FStructOnScope X(Struct), Y(Struct);
  const auto DecodeForComparison = [&](const TSharedPtr<FJsonValue>& Value, void* Memory) {
    // Hash identities and write elision retain the stored quaternion components;
    // orientation normalization is only appropriate for intent verification.
    if (bStoredValues && Kind == TEXT("quat"))
      return DecodeComponents(Struct, nullptr, Value, Memory, {TEXT("X"), TEXT("Y"), TEXT("Z"), TEXT("W")}, Error);
    return DecodeStructValue(Struct, Value, Memory, Error);
  };
  if (!DecodeForComparison(Left, X.GetStructMemory()) || !DecodeForComparison(Right, Y.GetStructMemory())) return Decide(false);
  if (!bStoredValues) {
    FString ComparisonSemantics;
    if (Kind == TEXT("vector3") && Definition
        && Definition->TryGetStringField(TEXT("comparison_semantics"), ComparisonSemantics)
        && ComparisonSemantics == TransformVectorComparison) {
      const auto Read = [Struct](const uint8* Ptr) {
        return Struct == TVariantStructure<FVector3f>::Get()
            ? FVector(*reinterpret_cast<const FVector3f*>(Ptr)) : *reinterpret_cast<const FVector*>(Ptr);
      };
      return CompareTransformVector(Read(X.GetStructMemory()), Read(Y.GetStructMemory()), Left, Right);
    }
    if (Kind == TEXT("rotator")) {
      const auto Read = [Struct](const uint8* Ptr) {
        return Struct == TVariantStructure<FRotator3f>::Get()
            ? FQuat(reinterpret_cast<const FRotator3f*>(Ptr)->Quaternion())
            : reinterpret_cast<const FRotator*>(Ptr)->Quaternion();
      };
      return Decide(AreRotationsEquivalent(Read(X.GetStructMemory()), Read(Y.GetStructMemory()), Error));
    }
    if (Kind == TEXT("quat")) {
      const auto Read = [Struct](const uint8* Ptr) {
        return Struct == TVariantStructure<FQuat4f>::Get() ? FQuat(*reinterpret_cast<const FQuat4f*>(Ptr))
            : *reinterpret_cast<const FQuat*>(Ptr);
      };
      return Decide(AreRotationsEquivalent(Read(X.GetStructMemory()), Read(Y.GetStructMemory()), Error));
    }
    if (Kind == TEXT("transform")) {
      const auto Read = [Struct](const uint8* Ptr) {
        return Struct == TVariantStructure<FTransform3f>::Get() ? FTransform(*reinterpret_cast<const FTransform3f*>(Ptr))
            : *reinterpret_cast<const FTransform*>(Ptr);
      };
      const FTransform TX = Read(X.GetStructMemory()), TY = Read(Y.GetStructMemory());
      const auto EncodedX = EncodeStructValue(Struct, X.GetStructMemory(), nullptr, {})->AsObject();
      const auto EncodedY = EncodeStructValue(Struct, Y.GetStructMemory(), nullptr, {})->AsObject();
      for (const TCHAR* Field : {TEXT("translation"), TEXT("scale")}) {
        const bool bTranslation = FCString::Strcmp(Field, TEXT("translation")) == 0;
        const auto Compared = CompareTransformVector(
            bTranslation ? TX.GetTranslation() : TX.GetScale3D(),
            bTranslation ? TY.GetTranslation() : TY.GetScale3D(),
            EncodedX->TryGetField(Field), EncodedY->TryGetField(Field));
        if (!Compared.bEquivalent) return Compared.WithPrefix(FString(TEXT("/")) + Field);
      }
      // Orientation is one semantic value; keep its necessary components together.
      if (!AreRotationsEquivalent(TX.GetRotation(), TY.GetRotation(), Error)) {
        if (!Error.IsEmpty()) return FValueComparison::Failed(Error).WithPrefix(TEXT("/rotation"));
        return FValueComparison::Different(TEXT("/rotation"),
            EncodedX->TryGetField(TEXT("rotation")), EncodedY->TryGetField(TEXT("rotation")));
      }
      return FValueComparison();
    }
  }
  // All other specialized struct shapes reuse their authoring decoder and encoder.
  auto Compared = UE::UAI::StateSemantic::CanonicalJson::CompareJsonValues(
      EncodeStructValue(Struct, X.GetStructMemory(), nullptr, {}),
      EncodeStructValue(Struct, Y.GetStructMemory(), nullptr, {}));
  // Transform components use arrays; standalone math values use a components object.
  if (Left->Type == EJson::Array && Compared.Path.StartsWith(TEXT("/components")))
    Compared.Path.RightChopInline(11);
  return Compared;
}

static FValueComparison ComparePropertyValuesInternal(FProperty* Property, const void* LeftValuePtr,
    const void* RightValuePtr, UObject* Owner, const FOptions& Options, bool bSingleElement = false) {
  if (!Property || !LeftValuePtr || !RightValuePtr) {
    auto Error = FDiagnostic::Failure(TEXT("property_comparison_storage_missing"),
        TEXT("Native property comparison requires a reflected property and both value storage locations."), TEXT("semantic_compare"));
    const auto Availability = MakeShared<FJsonObject>();
    Availability->SetStringField(TEXT("property"), Property ? Property->GetPathName() : TEXT("absent"));
    Availability->SetBoolField(TEXT("expected_storage_present"), LeftValuePtr != nullptr);
    Availability->SetBoolField(TEXT("actual_storage_present"), RightValuePtr != nullptr);
    Error.ActualValue = MakeShared<FJsonValueObject>(Availability);
    return FValueComparison::Failed(MoveTemp(Error));
  }
  const auto Compare = [&](FProperty* Child, const void* Left, const void* Right) {
    return ComparePropertyValuesInternal(Child, Left, Right, Owner, Options);
  };
  const auto Missing = [&](FString Path, FProperty* Child, const void* Left, const void* Right, bool bSingle = false) {
    return FValueComparison::Different(MoveTemp(Path),
        Left ? Encode(Child, Left, Owner, Options, 0, nullptr, bSingle) : nullptr,
        Right ? Encode(Child, Right, Owner, Options, 0, nullptr, bSingle) : nullptr);
  };
  if (!bSingleElement && Property->ArrayDim > 1) {
    for (int32 Index = 0; Index < Property->ArrayDim; ++Index) {
      const auto Compared = ComparePropertyValuesInternal(Property,
          static_cast<const uint8*>(LeftValuePtr) + Index * Property->ElementSize,
          static_cast<const uint8*>(RightValuePtr) + Index * Property->ElementSize, Owner, Options, true);
      if (!Compared.bEquivalent) return Compared.WithPrefix(FString::Printf(TEXT("/items/%d"), Index));
    }
    return FValueComparison();
  }
  if (const FArrayProperty* Array = CastField<FArrayProperty>(Property)) {
    FScriptArrayHelper Left(Array, LeftValuePtr), Right(Array, RightValuePtr);
    for (int32 Index = 0; Index < FMath::Min(Left.Num(), Right.Num()); ++Index) {
      const auto Compared = Compare(Array->Inner, Left.GetRawPtr(Index), Right.GetRawPtr(Index));
      if (!Compared.bEquivalent) return Compared.WithPrefix(FString::Printf(TEXT("/items/%d"), Index));
    }
    if (Left.Num() != Right.Num()) {
      const int32 Index = FMath::Min(Left.Num(), Right.Num());
      return Missing(FString::Printf(TEXT("/items/%d"), Index), Array->Inner,
          Index < Left.Num() ? Left.GetRawPtr(Index) : nullptr, Index < Right.Num() ? Right.GetRawPtr(Index) : nullptr);
    }
    return FValueComparison();
  }
  if (const FSetProperty* Set = CastField<FSetProperty>(Property)) {
    FScriptSetHelper Left(Set, LeftValuePtr), Right(Set, RightValuePtr);
    TSet<int32> Matched;
    int32 PublicIndex = 0;
    for (int32 LeftIndex = 0; LeftIndex < Left.GetMaxIndex(); ++LeftIndex) {
      if (!Left.IsValidIndex(LeftIndex)) continue;
      int32 Match = INDEX_NONE;
      for (int32 RightIndex = 0; RightIndex < Right.GetMaxIndex(); ++RightIndex)
        if (Right.IsValidIndex(RightIndex) && Set->ElementProp->Identical(
            Left.GetElementPtr(LeftIndex), Right.GetElementPtr(RightIndex), PPF_None)) {
          Match = RightIndex; break;
        }
      const FString Path = FString::Printf(TEXT("/items/%d"), PublicIndex++);
      if (Match == INDEX_NONE) return Missing(Path, Set->ElementProp, Left.GetElementPtr(LeftIndex), nullptr);
      Matched.Add(Match);
      const auto Compared = Compare(Set->ElementProp, Left.GetElementPtr(LeftIndex), Right.GetElementPtr(Match));
      if (!Compared.bEquivalent) return Compared.WithPrefix(Path);
    }
    PublicIndex = 0;
    for (int32 Index = 0; Index < Right.GetMaxIndex(); ++Index) {
      if (!Right.IsValidIndex(Index)) continue;
      if (!Matched.Contains(Index)) return Missing(FString::Printf(TEXT("/items/%d"), PublicIndex),
          Set->ElementProp, nullptr, Right.GetElementPtr(Index));
      ++PublicIndex;
    }
    return FValueComparison();
  }
  if (const FMapProperty* Map = CastField<FMapProperty>(Property)) {
    FScriptMapHelper Left(Map, LeftValuePtr), Right(Map, RightValuePtr);
    TSet<int32> Matched;
    int32 PublicIndex = 0;
    for (int32 LeftIndex = 0; LeftIndex < Left.GetMaxIndex(); ++LeftIndex) {
      if (!Left.IsValidIndex(LeftIndex)) continue;
      int32 Match = INDEX_NONE;
      for (int32 RightIndex = 0; RightIndex < Right.GetMaxIndex(); ++RightIndex)
        if (Right.IsValidIndex(RightIndex) && Map->KeyProp->Identical(
            Left.GetKeyPtr(LeftIndex), Right.GetKeyPtr(RightIndex), PPF_None)) {
          Match = RightIndex; break;
        }
      const FString Path = FString::Printf(TEXT("/entries/%d"), PublicIndex++);
      if (Match == INDEX_NONE) return Missing(Path + TEXT("/key"), Map->KeyProp, Left.GetKeyPtr(LeftIndex), nullptr);
      Matched.Add(Match);
      const auto Key = Compare(Map->KeyProp, Left.GetKeyPtr(LeftIndex), Right.GetKeyPtr(Match));
      if (!Key.bEquivalent) return Key.WithPrefix(Path + TEXT("/key"));
      const auto Value = Compare(Map->ValueProp, Left.GetValuePtr(LeftIndex), Right.GetValuePtr(Match));
      if (!Value.bEquivalent) return Value.WithPrefix(Path + TEXT("/value"));
    }
    PublicIndex = 0;
    for (int32 Index = 0; Index < Right.GetMaxIndex(); ++Index) {
      if (!Right.IsValidIndex(Index)) continue;
      if (!Matched.Contains(Index)) return Missing(FString::Printf(TEXT("/entries/%d/key"), PublicIndex),
          Map->KeyProp, nullptr, Right.GetKeyPtr(Index));
      ++PublicIndex;
    }
    return FValueComparison();
  }
  if (const FOptionalProperty* Optional = CastField<FOptionalProperty>(Property)) {
    if (Optional->IsSet(LeftValuePtr) != Optional->IsSet(RightValuePtr))
      return FValueComparison::Different(TEXT("/is_set"), MakeShared<FJsonValueBoolean>(Optional->IsSet(LeftValuePtr)),
          MakeShared<FJsonValueBoolean>(Optional->IsSet(RightValuePtr)));
    if (!Optional->IsSet(LeftValuePtr)) return FValueComparison();
    return Compare(Optional->GetValueProperty(), Optional->GetValuePointerForRead(LeftValuePtr),
        Optional->GetValuePointerForRead(RightValuePtr)).WithPrefix(TEXT("/value"));
  }
  if (const FStructProperty* Struct = CastField<FStructProperty>(Property)) {
    const FString Kind = StructKind(Struct->Struct);
    const UScriptStruct* FieldsType = Struct->Struct;
    const void* LeftFields = LeftValuePtr;
    const void* RightFields = RightValuePtr;
    if (Kind == TEXT("instanced_struct")) {
      const auto& Left = *static_cast<const FInstancedStruct*>(LeftValuePtr);
      const auto& Right = *static_cast<const FInstancedStruct*>(RightValuePtr);
      if (Left.GetScriptStruct() != Right.GetScriptStruct()) {
        const auto TypeValue = [](const UScriptStruct* Type) -> TSharedPtr<FJsonValue> {
          if (Type) return MakeShared<FJsonValueString>(Type->GetPathName());
          return MakeShared<FJsonValueNull>();
        };
        return FValueComparison::Different(TEXT("/struct_path"), TypeValue(Left.GetScriptStruct()), TypeValue(Right.GetScriptStruct()));
      }
      FieldsType = Left.GetScriptStruct(); LeftFields = Left.GetMemory(); RightFields = Right.GetMemory();
      if (!FieldsType) return FValueComparison();
    }
    if (Kind == TEXT("struct") || Kind == TEXT("instanced_struct"))
      return CompareReflectedStructFields(FieldsType, LeftFields, RightFields, Owner, Options);
  }
  TSharedPtr<FJsonObject> Definition;
  const auto Left = Encode(Property, LeftValuePtr, Owner, Options, 0, &Definition, bSingleElement);
  const auto Right = Encode(Property, RightValuePtr, Owner, Options, 0, nullptr, bSingleElement);
  if (!IsReliableCanonicalValue(Property, Left) || !IsReliableCanonicalValue(Property, Right)) {
    auto Error = FDiagnostic::Failure(TEXT("property_comparison_encoding_unavailable"),
        FString::Printf(TEXT("Property '%s' could not be encoded reliably for semantic comparison."), *Property->GetPathName()),
        TEXT("semantic_compare"));
    const auto Inputs = MakeShared<FJsonObject>();
    if (Left) Inputs->SetField(TEXT("expected_input"), Left);
    if (Right) Inputs->SetField(TEXT("actual_input"), Right);
    Error.ActualValue = MakeShared<FJsonValueObject>(Inputs);
    return FValueComparison::Failed(MoveTemp(Error));
  }
  if (const auto Compared = CompareCanonicalValues(BuildTypeDescriptor(Property, bSingleElement),
      Definition, Left, Right, Options.bCompareStoredValues); Compared.IsSet()) return Compared.GetValue();
  return UE::UAI::StateSemantic::CanonicalJson::CompareJsonValues(Left, Right);
}

FValueComparison ComparePropertyValues(FProperty* Property, const void* ExpectedValuePtr,
    const void* ActualValuePtr, UObject* Owner, const FOptions& Options) {
  if (Property && ExpectedValuePtr && ActualValuePtr)
    if (const auto Member = CompareEffectiveMember(Property, ExpectedValuePtr, Options); Member.IsSet())
      return Member.GetValue();
  return ComparePropertyValuesInternal(Property, ExpectedValuePtr, ActualValuePtr, Owner, Options);
}

bool AreSemanticallyEquivalent(FProperty* Property, const void* LeftValuePtr,
    const void* RightValuePtr, UObject* Owner, const FOptions& Options) {
  // Existing predicate callers supply current storage before requested storage.
  return ComparePropertyValues(Property, RightValuePtr, LeftValuePtr, Owner, Options).bEquivalent;
}

bool VerifyValueIntent(FProperty* Property, const TSharedPtr<FJsonValue>& Intent,
    const void* ActualValuePtr, UObject* Owner, const FOptions& Options,
    FDiagnostic& OutError, bool bSingleFixedElement) {
  if (!Property || !ActualValuePtr || !Intent.IsValid())
    return Fail(OutError, TEXT("property_intent_value_missing"), TEXT("Intent verification requires the reflected property, authored value and native readback storage."));
  void* Expected = FMemory::Malloc(Property->GetSize(), Property->GetMinAlignment());
  Property->InitializeValue(Expected);
  if (bSingleFixedElement) Property->CopySingleValue(Expected, ActualValuePtr);
  else Property->CopyCompleteValue(Expected, ActualValuePtr);
  const bool bDecoded = Decode(Property, Intent, Expected, Owner, Options, OutError, 0, bSingleFixedElement);
  FValueComparison Compared;
  if (bDecoded) Compared = bSingleFixedElement
      ? ComparePropertyValuesInternal(Property, Expected, ActualValuePtr, Owner, Options, true)
      : ComparePropertyValues(Property, Expected, ActualValuePtr, Owner, Options);
  Property->DestroyValue(Expected);
  FMemory::Free(Expected);
  if (!bDecoded) return false;
  if (!Compared.bEquivalent) {
    Fail(OutError, TEXT("readback_mismatch"), TEXT("The native readback differs from the authored property value."));
    Compared.ApplyTo(OutError);
  }
  return Compared.bEquivalent;
}

FValueComparison CompareReflectedStructFields(const UScriptStruct* Struct, const void* Left,
    const void* Right, UObject* Owner, const FOptions& Options) {
  if (!Struct || !Left || !Right) {
    auto Error = FDiagnostic::Failure(TEXT("struct_comparison_storage_missing"),
        TEXT("Reflected struct comparison requires a struct declaration and both value storage locations."), TEXT("semantic_compare"));
    const auto Availability = MakeShared<FJsonObject>();
    Availability->SetStringField(TEXT("struct"), Struct ? Struct->GetPathName() : TEXT("absent"));
    Availability->SetBoolField(TEXT("expected_storage_present"), Left != nullptr);
    Availability->SetBoolField(TEXT("actual_storage_present"), Right != nullptr);
    Error.ActualValue = MakeShared<FJsonValueObject>(Availability);
    return FValueComparison::Failed(MoveTemp(Error));
  }
  if (const auto Effective = CompareEffectiveStruct(Struct, Left, Right, Options); Effective.IsSet())
    return Effective.GetValue();
  TOptional<FEnvTraceData> CanonicalLeft, CanonicalRight;
  if (UeAgentEnvTraceDataValue::IsType(Struct)) {
    CanonicalLeft = UeAgentEnvTraceDataValue::Canonical(*static_cast<const FEnvTraceData*>(Left));
    CanonicalRight = UeAgentEnvTraceDataValue::Canonical(*static_cast<const FEnvTraceData*>(Right));
    Left = &CanonicalLeft.GetValue(); Right = &CanonicalRight.GetValue();
  }
  for (TFieldIterator<FProperty> It(Struct, EFieldIterationFlags::IncludeSuper); It; ++It) {
    FProperty* Field = *It;
    if (Field->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated) ||
        (Options.StructFieldFilter && !Options.StructFieldFilter(Struct, Field))) continue;
    const auto Compared = ComparePropertyValuesInternal(Field, Field->ContainerPtrToValuePtr<void>(Left),
        Field->ContainerPtrToValuePtr<void>(Right), Owner, Options);
    if (!Compared.bEquivalent) return Compared.WithPrefix(TEXT("/fields/") +
        Field->GetName().Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")));
  }
  return FValueComparison();
}

bool AreReflectedStructFieldsEquivalent(const UScriptStruct* Struct, const void* Left,
    const void* Right, UObject* Owner, const FOptions& Options) {
  return CompareReflectedStructFields(Struct, Left, Right, Owner, Options).bEquivalent;
}

bool DecodeStructValue(UScriptStruct *Struct,
                       const TSharedPtr<FJsonValue> &Value, void *OutValuePtr,
                       FDiagnostic& OutError) {
  return DecodeStructValueInternal(Struct, nullptr, Value, OutValuePtr, nullptr,
                                   FOptions(), OutError, 0);
}

bool DecodeStructValue(UScriptStruct *Struct,
                       const TSharedPtr<FJsonValue> &Value, void *OutValuePtr,
                       UObject *Owner, const FOptions &Options,
                       FDiagnostic& OutError, int32 Depth) {
  return DecodeStructValueInternal(Struct, nullptr, Value, OutValuePtr, Owner,
                                   Options, OutError, Depth);
}

TSharedPtr<FJsonValue> EncodePropertyBag(const FInstancedPropertyBag &Bag,
                                         UObject *Owner,
                                         const FOptions &Options, int32 Depth,
                                         TSharedPtr<FJsonObject>* OutValueDefinition) {
  if (OutValueDefinition) OutValueDefinition->Reset();
  TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
  TArray<TSharedPtr<FJsonValue>> Schema;
  TSharedPtr<FJsonObject> Fields = MakeShared<FJsonObject>();
  TSharedPtr<FJsonObject> FieldDefinitions;
  const FInstancedStruct *Storage = GetPropertyBagStorage(&Bag);
  if (Bag.IsValid() && Bag.GetPropertyBagStruct() && Storage &&
      Storage->GetMemory()) {
    for (const FPropertyBagPropertyDesc &Desc :
         Bag.GetPropertyBagStruct()->GetPropertyDescs()) {
      if (!Desc.CachedProperty)
        continue;
      Schema.Add(MakeObjectValue(BuildPropertyBagSchemaEntry(Desc)));
      FProperty *Field = const_cast<FProperty *>(Desc.CachedProperty);
      TSharedPtr<FJsonObject> Definition;
      Fields->SetField(
          Desc.Name.ToString(),
          EncodeInternal(
              Field, Field->ContainerPtrToValuePtr<void>(Storage->GetMemory()),
              Owner, Options, Depth + 1, false,
              OutValueDefinition ? &Definition : nullptr));
      if (OutValueDefinition && Options.bCaptureStructFieldDescriptors)
        Definition = CaptureFieldDefinition(Field, Definition, Options.bShareImmutableFieldDefinitions);
      if (Definition.IsValid()) {
        if (!FieldDefinitions.IsValid()) FieldDefinitions = MakeShared<FJsonObject>();
        FieldDefinitions->SetObjectField(Desc.Name.ToString(), Definition);
      }
    }
  }
  Object->SetArrayField(TEXT("schema"), Schema);
  if (OutValueDefinition) {
    *OutValueDefinition = MakeShared<FJsonObject>();
    (*OutValueDefinition)->SetStringField(TEXT("builtin"), TEXT("property_bag"));
    if (FieldDefinitions.IsValid())
      (*OutValueDefinition)->SetObjectField(TEXT("fields"), FieldDefinitions);
  }
  Object->SetObjectField(TEXT("fields"), Fields);
  return MakeObjectValue(Object);
}

bool PreparePropertyBagSchema(const TSharedPtr<FJsonValue> &Value,
                       FInstancedPropertyBag &OutBag,
                       const FOptions &Options, FDiagnostic& OutError,
                       int32 Depth) {
  TSharedPtr<FJsonObject> Object;
  const TArray<TSharedPtr<FJsonValue>> *Schema = nullptr;
  const TSharedPtr<FJsonObject> *Fields = nullptr;
  if (!GetObject(Value, Object, OutError) ||
      !OnlyFields(Object, {TEXT("schema"), TEXT("fields")}, TEXT("invalid_property_bag_shape"), OutError) ||
      !RequiredArray(Object, TEXT("schema"), Schema, TEXT("invalid_property_bag_shape"), OutError)) return false;
  if (!Object->TryGetObjectField(TEXT("fields"), Fields) || !Fields ||
      !Fields->IsValid()) {
    return ValueFailure(OutError, TEXT("invalid_property_bag_shape"), TEXT("A property bag value requires a fields object."), Object->TryGetField(TEXT("fields")), TEXT("object"), TEXT("/fields"));
  }
  TArray<FPropertyBagPropertyDesc> Descs;
  TSet<FName> Names;
  for (int32 Index = 0; Index < Schema->Num(); ++Index) {
    const auto& SchemaValue = (*Schema)[Index];
    TSharedPtr<FJsonObject> Entry;
    FPropertyBagPropertyDesc Desc;
    if (!GetObject(SchemaValue, Entry, OutError) || !ParsePropertyBagSchemaEntry(Entry, Desc, OutError)) {
      PrefixValueFailure(OutError, FString::Printf(TEXT("/schema/%d"), Index), FString::Printf(TEXT("schema[%d]"), Index));
      return false;
    }
    if (Names.Contains(Desc.Name))
      return ValueFailure(OutError, TEXT("duplicate_property_bag_field"), TEXT("The property bag schema declares the same field name more than once."),
          MakeShared<FJsonValueString>(Desc.Name.ToString()), TEXT("unique field name"), FString::Printf(TEXT("/schema/%d/name"), Index));
    // The public schema describes values/types, not native binding identity.
    // Match the native AddProperties name-based update contract while retaining
    // metadata that was never part of the public edit surface.
    if (const FPropertyBagPropertyDesc* Existing = OutBag.FindPropertyDescByName(Desc.Name)) {
      Desc.ID = Existing->ID;
      Desc.PropertyFlags = Existing->PropertyFlags;
#if WITH_EDITORONLY_DATA
      Desc.MetaData = Existing->MetaData;
      Desc.MetaClass = Existing->MetaClass;
#endif
    }
    Names.Add(Desc.Name);
    Descs.Add(MoveTemp(Desc));
  }
  for (const FPropertyBagPropertyDesc& Desc : Descs)
    if (!(*Fields)->HasField(Desc.Name.ToString()))
      return ValueFailure(OutError, FString::Printf(TEXT("missing_property_bag_field_%s"), *Desc.Name.ToString()), TEXT("A field declared in the property bag schema has no value."),
          nullptr, TEXT("schema field value"), TEXT("/fields") + FieldPointer(Desc.Name.ToString()));
  for (const auto& Pair : (*Fields)->Values)
    if (!Names.Contains(FName(*Pair.Key)))
      return ValueFailure(OutError, FString::Printf(TEXT("unknown_property_bag_field_%s"), *Pair.Key), TEXT("The property bag field is not declared in its schema."),
          MakeShared<FJsonValueString>(Pair.Key), TEXT("declared schema field"), TEXT("/fields") + FieldPointer(Pair.Key));
  if ((*Fields)->Values.Num() != Names.Num())
    return ValueFailure(OutError, TEXT("property_bag_schema_fields_mismatch"), TEXT("The property bag field names collapse to fewer distinct native names than the supplied fields."),
        MakeShared<FJsonValueNumber>((*Fields)->Values.Num()), FString::FromInt(Names.Num()), TEXT("/fields"));
  // Resolve omitted native field IDs before materializing the value's schema.
  // GetOrCreateFromDescs hashes its input before allocating missing IDs; using
  // that provisional type directly gives it a different identity on reload.
  // Reuse the native descriptor factory's allocated IDs (and cache semantics),
  // then construct from the complete descriptors just as deserialization does.
  // Existing IDs and non-public metadata above remain unchanged.
  if (Descs.ContainsByPredicate([](const FPropertyBagPropertyDesc& Desc) { return !Desc.ID.IsValid(); })) {
    const UPropertyBag* ResolvedSchema = UPropertyBag::GetOrCreateFromDescs(Descs);
    if (!ResolvedSchema)
      return Fail(OutError, TEXT("property_bag_schema_creation_failed"), TEXT("Unreal could not create the property bag schema from the supplied descriptors."));
    Descs = ResolvedSchema->GetPropertyDescs();
  }
  FInstancedPropertyBag Decoded;
  if (!Descs.IsEmpty() &&
      Decoded.AddProperties(Descs) != EPropertyBagAlterationResult::Success) {
    return Fail(OutError, TEXT("property_bag_schema_creation_failed"), TEXT("Unreal could not create the property bag schema from the supplied descriptors."));
  }
  // Seed surviving values so recursive preparation and decoding share native identities.
  Decoded.CopyMatchingValuesByID(OutBag);
  FInstancedStruct *Storage = GetMutablePropertyBagStorage(&Decoded);
  if (!Descs.IsEmpty() && (!Decoded.GetPropertyBagStruct() || !Storage ||
                           !Storage->GetMutableMemory())) {
    return Fail(OutError, TEXT("property_bag_storage_unavailable"), TEXT("The prepared property bag has no writable native storage."));
  }
  OutBag = MoveTemp(Decoded);
  return true;
}

bool DecodePropertyBag(const TSharedPtr<FJsonValue> &Value,
                       FInstancedPropertyBag &OutBag, UObject *Owner,
                       const FOptions &Options, FDiagnostic& OutError,
                       int32 Depth) {
  FInstancedPropertyBag Decoded = OutBag;
  if (!PreparePropertyBagSchema(Value, Decoded, Options, OutError, Depth)) return false;
  const auto Fields = Value->AsObject()->GetObjectField(TEXT("fields"));
  FInstancedStruct* Storage = GetMutablePropertyBagStorage(&Decoded);
  if (Decoded.GetPropertyBagStruct()) {
    for (const FPropertyBagPropertyDesc &Desc :
         Decoded.GetPropertyBagStruct()->GetPropertyDescs()) {
      const TSharedPtr<FJsonValue> FieldValue =
          Fields->TryGetField(Desc.Name.ToString());
      if (!FieldValue.IsValid())
        return ValueFailure(OutError, FString::Printf(TEXT("missing_property_bag_field_%s"), *Desc.Name.ToString()), TEXT("A prepared property bag field has no authored value."),
            FieldValue, TEXT("schema field value"), TEXT("/fields") + FieldPointer(Desc.Name.ToString()));
      FProperty *Field = const_cast<FProperty *>(Desc.CachedProperty);
      if (!Field) {
        Fail(OutError, TEXT("property_bag_storage_unavailable"), TEXT("The prepared property bag field has no reflected property."));
        PrefixValueFailure(OutError, TEXT("/fields") + FieldPointer(Desc.Name.ToString()), Desc.Name.ToString());
        return false;
      }
      if (!DecodeInternal(
              Field, FieldValue,
              Field->ContainerPtrToValuePtr<void>(Storage->GetMutableMemory()),
              Owner, Options, OutError, Depth + 1, false)) {
        PrefixValueFailure(OutError, TEXT("/fields/") + Desc.Name.ToString().Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")), Desc.Name.ToString());
        return false;
      }
    }
  }
  OutBag = MoveTemp(Decoded);
  return true;
}

bool IsReliableCanonicalValue(const FProperty *Property,
                              const TSharedPtr<FJsonValue> &Value,
                              FDiagnostic* OutReason) {
  FDiagnostic Error;
  if (!Property || !Value.IsValid() || Value->Type == EJson::Null ||
      GetValueKind(Property) == TEXT("unavailable"))
    ValueFailure(Error, TEXT("canonical_value_unavailable"), TEXT("A reliable canonical value requires a supported property and a non-null value."), Value, TEXT("supported canonical property value"));
  else if (Value->Type != EJson::Object)
    ValueFailure(Error, TEXT("canonical_value_must_be_object"), TEXT("The canonical property value must be an object."), Value, TEXT("object"));
  else if (IsTruncatedValue(Value))
    Fail(Error, TEXT("canonical_value_truncated_or_incomplete"), TEXT("The canonical property value contains truncated or unavailable data."));
  if (OutReason)
    *OutReason = Error;
  return Error.IsEmpty();
}
} // namespace UeAgentPropertyValueCodec
