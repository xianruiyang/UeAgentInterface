// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Utilities/AuthoringCore/UeAgentPropertyReadScope.h"

#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringEncodedTypes.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringReflectionDescriptors.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Utilities/UeAgentPropertySupportProof.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeRWLock.h"
#include "Misc/ScopeExit.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"
#endif
#include <type_traits>
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

namespace UeAgentAuthoringState {
static bool SameStateDescriptorAuthoringContent(const UeAgentAuthoringTypeDescriptors::FDescriptor&,
    const UeAgentAuthoringTypeDescriptors::FDescriptor&);
static bool SameStateIdentityAuthoringContent(const UeAgentAuthoringIdentity::FCanonicalIdentity&,
    const UeAgentAuthoringIdentity::FCanonicalIdentity&);
namespace {
// Aggregate timing observes the existing synchronous capture, including an
// explicitly selected complete authoring export; no per-entry log is produced.
struct FBoundedCaptureTiming {
  int32 PropertyReads = 0;
  int32 SelectedPropertyReads = 0;
  int32 FreshEncodedTransfers = 0, OrderedNormalizations = 0;
  int32 OwnedArrayCandidates = 0, EmptyOwnedArrays = 0, ReusedEmptyArrays = 0;
  int32 ReusedScalars = 0;
  int32 DirectLiterals = 0, DirectEnums = 0, DirectIntegerStrings = 0, DirectNumbers = 0, MovedResolvedIdentities = 0;
  int32 DirectClosedShapes = 0, DirectShapeNumbers = 0;
  int32 DirectReferences = 0, DirectSequences = 0, BorrowedResolvedIdentities = 0, DirectBitmasks = 0;
  int64 RetiredTypeRecords = 0;
  TMap<FString, int32> JsonKinds;
  double EmptyProofSeconds = 0, EmptyMetadataProofSeconds = 0;
  double ScalarProofSeconds = 0, ScalarMetadataProofSeconds = 0;
  double ResolveSeconds = 0, DescriptorSeconds = 0, EncodeSeconds = 0, NativeCopySeconds = 0;
  double ImmutableEncodeSeconds = 0, JsonEncodeSeconds = 0;
};
static thread_local FBoundedCaptureTiming* ActiveCaptureTiming = nullptr;
static thread_local bool bAuthoringExportCapture = false;
static thread_local bool bAuthoringExportLiterals = false;

FString NormalizeId(FString Value) {
  Value.TrimStartAndEndInline();
  Value.ToLowerInline();
  return Value;
}

void NormalizeList(TArray<FString> &Values) {
  for (FString &Value : Values)
    Value = NormalizeId(MoveTemp(Value));
  Values.RemoveAll([](const FString &Value) { return Value.IsEmpty(); });
  Values.Sort();
  for (int32 Index = Values.Num() - 1; Index > 0; --Index) {
    if (Values[Index] == Values[Index - 1])
      Values.RemoveAt(Index);
  }
}

FStateAccessorDescriptor Normalize(FStateAccessorDescriptor Descriptor) {
  Descriptor.AccessorId = NormalizeId(MoveTemp(Descriptor.AccessorId));
  NormalizeList(Descriptor.TargetKinds);
  NormalizeList(Descriptor.CapabilityTags);
  return Descriptor;
}

FDiagnostic StateFailure(const TCHAR* Code, const TCHAR* Reason, const FString& Field,
    const TSharedPtr<FJsonValue>& Expected = nullptr, const TSharedPtr<FJsonValue>& Actual = nullptr) {
  auto Error = FDiagnostic::Failure(Code, Reason, TEXT("state_snapshot"));
  Error.SourceKind = TEXT("native_result");
  Error.PropertyPath = Field;
  Error.ExpectedValue = Expected; Error.ActualValue = Actual;
  return Error;
}
FDiagnostic MissingStateField(const TCHAR* Code, const TCHAR* Reason, const FString& Field) {
  auto Error = StateFailure(Code, Reason, Field);
  Error.ExpectedValuePresent = true; Error.ActualValuePresent = false;
  return Error;
}
TSharedPtr<FJsonValue> DescriptorValue(const FString& Value) { return MakeShared<FJsonValueString>(Value); }
TSharedPtr<FJsonValue> DescriptorValue(int32 Value) { return MakeShared<FJsonValueNumber>(Value); }
template<typename T>
bool DescriptorEqual(const T& Expected, const T& Actual, const FString& Path, FDiagnostic* Error) {
  if (Expected == Actual) return true;
  if (Error) { Error->PropertyPath = Path; Error->ExpectedValue = DescriptorValue(Expected); Error->ActualValue = DescriptorValue(Actual); }
  return false;
}
bool DescriptorEqual(const TArray<FString>& Expected, const TArray<FString>& Actual, const FString& Path, FDiagnostic* Error) {
  if (!DescriptorEqual(Expected.Num(), Actual.Num(), Path + TEXT("/count"), Error)) return false;
  for (int32 Index = 0; Index < Expected.Num(); ++Index)
    if (!DescriptorEqual(Expected[Index], Actual[Index], Path + TEXT("/") + FString::FromInt(Index), Error)) return false;
  return true;
}
bool AccessorsEqual(const FStateAccessorDescriptor& Expected, const FStateAccessorDescriptor& Actual, FDiagnostic* Error) {
  return DescriptorEqual(Expected.AccessorId, Actual.AccessorId, TEXT("/accessor_id"), Error)
    && DescriptorEqual(Expected.TargetKinds, Actual.TargetKinds, TEXT("/target_kinds"), Error)
    && DescriptorEqual(Expected.CapabilityTags, Actual.CapabilityTags, TEXT("/capability_tags"), Error);
}

bool ValidateAccessor(const FStateAccessorDescriptor &Descriptor,
                      const FStateAccessor &Accessor, UE::UAI::Contract::FDiagnostic& OutError) {
  if (Descriptor.AccessorId.IsEmpty())
    OutError = MissingStateField(TEXT("state_accessor_id_missing"), TEXT("The state accessor identifier is empty."), TEXT("accessor_id"));
  else if (Descriptor.TargetKinds.IsEmpty())
    OutError = MissingStateField(TEXT("state_accessor_target_kinds_missing"), TEXT("The state accessor declares no supported target kinds."), TEXT("target_kinds"));
  else if (!Accessor)
    OutError = MissingStateField(TEXT("state_accessor_callback_missing"), TEXT("The state accessor has no read callback."), TEXT("callback"));
  else {
    OutError.Reset();
    return true;
  }
  OutError.Stage = TEXT("semantic_registry");
  OutError.CandidateName = Descriptor.AccessorId;
  return false;
}

TSharedPtr<FJsonValue> CloneJsonValue(const TSharedPtr<FJsonValue> &Value) {
  if (!Value.IsValid())
    return nullptr;

  auto MakeUntypedCloneShell = [](const TSharedPtr<FJsonValue> &Source) {
    if (!Source.IsValid())
      return TSharedPtr<FJsonValue>();
    switch (Source->Type) {
    case EJson::String:
      return TSharedPtr<FJsonValue>(
          MakeShared<FJsonValueString>(Source->AsString()));
    case EJson::Number:
      if (Source->PreferStringRepresentation()) {
        FString Lexeme;
        if (Source->TryGetString(Lexeme))
          return TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumberString>(Lexeme));
      }
      return TSharedPtr<FJsonValue>(
          MakeShared<FJsonValueNumber>(Source->AsNumber()));
    case EJson::Boolean:
      return TSharedPtr<FJsonValue>(
          MakeShared<FJsonValueBoolean>(Source->AsBool()));
    case EJson::Array:
      return TSharedPtr<FJsonValue>(MakeShared<FJsonValueArray>(
          TArray<TSharedPtr<FJsonValue>>()));
    case EJson::Object:
      return TSharedPtr<FJsonValue>(
          MakeShared<FJsonValueObject>(MakeShared<FJsonObject>()));
    case EJson::Null:
    case EJson::None:
    default:
      return TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>());
    }
  };

  auto MakeCloneShell = [&](const TSharedPtr<FJsonValue>& Source) {
    auto Clone = MakeUntypedCloneShell(Source);
    UeAgentAuthoringEncodedTypes::FScope::Copy(Source, Clone);
    return Clone;
  };
  TSharedPtr<FJsonValue> Root = MakeCloneShell(Value);
  if (Value->Type != EJson::Array && Value->Type != EJson::Object)
    return Root;

  struct FCloneFrame {
    TSharedPtr<FJsonValue> Source;
    TSharedPtr<FJsonValue> Destination;
    TArray<FString> ObjectKeys;
    int32 NextIndex = 0;
    bool bEntered = false;
  };

  TMap<const FJsonObject *, TSharedPtr<FJsonValue>> ObjectMemo;
  TMap<const TArray<TSharedPtr<FJsonValue>> *, TSharedPtr<FJsonValue>>
      ArrayMemo;
  TSet<const FJsonObject *> ActiveObjects;
  TSet<const TArray<TSharedPtr<FJsonValue>> *> ActiveArrays;
  if (Value->Type == EJson::Object)
    ObjectMemo.Add(Value->AsObject().Get(), Root);
  else
    ArrayMemo.Add(&Value->AsArray(), Root);

  TArray<FCloneFrame> Stack;
  Stack.Add({Value, Root});
  while (!Stack.IsEmpty()) {
    FCloneFrame &Frame = Stack.Last();
    if (!Frame.bEntered) {
      Frame.bEntered = true;
      if (Frame.Source->Type == EJson::Object) {
        const FJsonObject *Identity = Frame.Source->AsObject().Get();
        if (!Identity || ActiveObjects.Contains(Identity))
          return nullptr;
        ActiveObjects.Add(Identity);
        Frame.Source->AsObject()->Values.GetKeys(Frame.ObjectKeys);
      } else {
        const TArray<TSharedPtr<FJsonValue>> *Identity =
            &Frame.Source->AsArray();
        if (ActiveArrays.Contains(Identity))
          return nullptr;
        ActiveArrays.Add(Identity);
        TArray<TSharedPtr<FJsonValue>> *DestinationItems = nullptr;
        if (!Frame.Destination->TryGetArray(DestinationItems) ||
            !DestinationItems)
          return nullptr;
        DestinationItems->SetNum(Frame.Source->AsArray().Num());
      }
    }

    const int32 ItemCount =
        Frame.Source->Type == EJson::Object
            ? Frame.ObjectKeys.Num()
            : Frame.Source->AsArray().Num();
    if (Frame.NextIndex >= ItemCount) {
      if (Frame.Source->Type == EJson::Object)
        ActiveObjects.Remove(Frame.Source->AsObject().Get());
      else
        ActiveArrays.Remove(&Frame.Source->AsArray());
      Stack.Pop(EAllowShrinking::No);
      continue;
    }

    const int32 CurrentIndex = Frame.NextIndex++;
    FString Key;
    TSharedPtr<FJsonValue> SourceChild;
    if (Frame.Source->Type == EJson::Object) {
      Key = Frame.ObjectKeys[CurrentIndex];
      SourceChild = Frame.Source->AsObject()->TryGetField(Key);
    } else {
      SourceChild = Frame.Source->AsArray()[CurrentIndex];
    }

    TSharedPtr<FJsonValue> DestinationChild;
    bool bPushChild = false;
    if (!SourceChild.IsValid()) {
      DestinationChild.Reset();
    } else if (SourceChild->Type == EJson::Object) {
      const FJsonObject *Identity = SourceChild->AsObject().Get();
      if (!Identity || ActiveObjects.Contains(Identity))
        return nullptr;
      if (const TSharedPtr<FJsonValue> *Existing = ObjectMemo.Find(Identity)) {
        DestinationChild = *Existing;
      } else {
        DestinationChild = MakeCloneShell(SourceChild);
        ObjectMemo.Add(Identity, DestinationChild);
        bPushChild = true;
      }
    } else if (SourceChild->Type == EJson::Array) {
      const TArray<TSharedPtr<FJsonValue>> *Identity =
          &SourceChild->AsArray();
      if (ActiveArrays.Contains(Identity))
        return nullptr;
      if (const TSharedPtr<FJsonValue> *Existing = ArrayMemo.Find(Identity)) {
        DestinationChild = *Existing;
      } else {
        DestinationChild = MakeCloneShell(SourceChild);
        ArrayMemo.Add(Identity, DestinationChild);
        bPushChild = true;
      }
    } else {
      DestinationChild = MakeCloneShell(SourceChild);
    }

    if (Frame.Source->Type == EJson::Object) {
      Frame.Destination->AsObject()->SetField(Key, DestinationChild);
    } else {
      TArray<TSharedPtr<FJsonValue>> *DestinationItems = nullptr;
      if (!Frame.Destination->TryGetArray(DestinationItems) ||
          !DestinationItems)
        return nullptr;
      (*DestinationItems)[CurrentIndex] = DestinationChild;
    }
    if (bPushChild)
      Stack.Add({SourceChild, DestinationChild});
  }
  return Root;
}

TSharedPtr<FJsonObject> CloneJsonObject(const TSharedPtr<FJsonObject> &Object) {
  if (!Object.IsValid())
    return nullptr;
  const TSharedPtr<FJsonValue> Cloned =
      CloneJsonValue(MakeShared<FJsonValueObject>(Object));
  return Cloned.IsValid() && Cloned->Type == EJson::Object
             ? Cloned->AsObject()
             : nullptr;
}

template <typename TSink>
void AppendEscapedStableString(const FString &Value, TSink &Out) {
  int32 Start = 0;
  for (int32 Index = 0; Index < Value.Len(); ++Index) {
    const TCHAR Character = Value[Index];
    if (Character == TEXT('%') || Character == TEXT(':') ||
        Character == TEXT(';') || Character == TEXT('[') ||
        Character == TEXT(']') || Character == TEXT('{') ||
        Character == TEXT('}') || Character == TEXT(',') ||
        Character == TEXT('|') || Character == TEXT('\r') ||
        Character == TEXT('\n') || Character == TEXT('\t')) {
      if (Index > Start) Out += FStringView(*Value + Start, Index - Start);
      // The escaped alphabet above is ASCII. Preserve the original %04X
      // spelling without allocating a string for each reserved character.
      constexpr TCHAR Hex[] = TEXT("0123456789ABCDEF");
      const uint32 Code = static_cast<uint32>(Character);
      const TCHAR Escaped[] = {TEXT('%'), Hex[(Code >> 12) & 15], Hex[(Code >> 8) & 15], Hex[(Code >> 4) & 15], Hex[Code & 15]};
      Out += FStringView(Escaped, UE_ARRAY_COUNT(Escaped));
      Start = Index + 1;
    }
  }
  if (Start < Value.Len()) Out += FStringView(*Value + Start, Value.Len() - Start);
}

struct FStableMaterialViewSink {
  TFunctionRef<void(FStringView)> Append;
  void operator+=(FStringView Text) { Append(Text); }
  void operator+=(const FString& Text) { Append(FStringView(Text)); }
  void operator+=(const TCHAR* Text) { Append(FStringView(Text)); }
};
template<typename TSink> void AppendStableKey(const FString& Key, TSink& Out) {
  Out += TEXT("k:"); AppendEscapedStableString(Key, Out); Out += TEXT(";");
}
template<typename TSink> void AppendStableStringValue(const FString& Value, TSink& Out) {
  Out += TEXT("s:"); AppendEscapedStableString(Value, Out); Out += TEXT(";");
}
template<typename TSink> void AppendStableBoolean(bool Value, TSink& Out) {
  Out += Value ? TEXT("bt") : TEXT("bf");
}
template<typename TSink> void AppendStableNumber(const FString& Lexeme, bool bHasLegacyNumber,
    double LegacyNumber, TSink& Out) {
  FString Normalized;
  if (!UE::UAI::StateSemantic::TryNormalizeNumberLexeme(Lexeme, Normalized)) {
    Out += TEXT("invalid-number"); return;
  }
  // This is the original binding grammar shared by JSON and pure numbers.
  if (bHasLegacyNumber && FMath::IsFinite(LegacyNumber)) {
    const FString Legacy = FString::Printf(TEXT("%.17g"), LegacyNumber == 0.0 ? 0.0 : LegacyNumber);
    FString LegacyNormalized;
    if (UE::UAI::StateSemantic::TryNormalizeNumberLexeme(Legacy, LegacyNormalized)
        && LegacyNormalized == Normalized) Normalized = Legacy;
  }
  Out += TEXT("n"); Out += Normalized;
}

enum class EStableTaskType : uint8 {
    Value,
    Key,
    ExitObject,
    ExitArray,
  };
struct FStableTask {
    EStableTaskType Type = EStableTaskType::Value;
    TSharedPtr<FJsonValue> JsonValue;
    const FString *Key = nullptr;
    const FJsonObject *ObjectIdentity = nullptr;
    const TArray<TSharedPtr<FJsonValue>> *ArrayIdentity = nullptr;
  };

template <typename TSink>
void AppendStableJson(const TSharedPtr<FJsonValue> &Value, TSink &Out) {
  TSet<const FJsonObject *> ActiveObjects;
  TSet<const TArray<TSharedPtr<FJsonValue>> *> ActiveArrays;
  TArray<FStableTask> Stack;
  Stack.Add({EStableTaskType::Value, Value});
  while (!Stack.IsEmpty()) {
    FStableTask Task = Stack.Pop(EAllowShrinking::No);
    if (Task.Type == EStableTaskType::Key) {
      AppendStableKey(*Task.Key, Out);
      continue;
    }
    if (Task.Type == EStableTaskType::ExitObject) {
      ActiveObjects.Remove(Task.ObjectIdentity);
      Out += TEXT("}");
      continue;
    }
    if (Task.Type == EStableTaskType::ExitArray) {
      ActiveArrays.Remove(Task.ArrayIdentity);
      Out += TEXT("]");
      continue;
    }
    if (!Task.JsonValue.IsValid()) {
      Out += TEXT("invalid");
      continue;
    }
    switch (Task.JsonValue->Type) {
    case EJson::String:
      AppendStableStringValue(Task.JsonValue->AsString(), Out);
      break;
    case EJson::Number: {
      FString Lexeme;
      if (!UE::UAI::StateSemantic::CanonicalJson::TryGetNumberLexeme(*Task.JsonValue, Lexeme)) {
        Out += TEXT("invalid-number");
        break;
      }
      double LegacyNumber = 0.0;
      const bool bHasNumber = Task.JsonValue->TryGetNumber(LegacyNumber);
      AppendStableNumber(Lexeme, bHasNumber, LegacyNumber, Out);
      break;
    }
    case EJson::Boolean:
      AppendStableBoolean(Task.JsonValue->AsBool(), Out);
      break;
    case EJson::Array: {
      const TArray<TSharedPtr<FJsonValue>> *Identity =
          &Task.JsonValue->AsArray();
      if (ActiveArrays.Contains(Identity)) {
        Out += TEXT("invalid-cycle");
        break;
      }
      ActiveArrays.Add(Identity);
      Out += TEXT("a[");
      FStableTask Exit;
      Exit.Type = EStableTaskType::ExitArray;
      Exit.ArrayIdentity = Identity;
      Stack.Add(MoveTemp(Exit));
      for (int32 Index = Identity->Num() - 1; Index >= 0; --Index)
        Stack.Add({EStableTaskType::Value, (*Identity)[Index]});
      break;
    }
    case EJson::Object: {
      const FJsonObject *Identity = Task.JsonValue->AsObject().Get();
      if (!Identity || ActiveObjects.Contains(Identity)) {
        Out += TEXT("invalid-cycle");
        break;
      }
      ActiveObjects.Add(Identity);
      Out += TEXT("o{");
      FStableTask Exit;
      Exit.Type = EStableTaskType::ExitObject;
      Exit.ObjectIdentity = Identity;
      Stack.Add(MoveTemp(Exit));
      // The root value owns every object throughout this synchronous walk.
      // Sort borrowed keys with the original FString comparator/order; do not
      // copy each key and then allocate another escaped key task string.
      TArray<const FString *> Keys;
      Keys.Reserve(Identity->Values.Num());
      for (const auto &Pair : Identity->Values) Keys.Add(&Pair.Key);
      Keys.Sort([](const FString &Left, const FString &Right) { return Left < Right; });
      for (int32 Index = Keys.Num() - 1; Index >= 0; --Index) {
        const FString &Key = *Keys[Index];
        Stack.Add(
            {EStableTaskType::Value, Identity->TryGetField(Key)});
        FStableTask KeyTask;
        KeyTask.Type = EStableTaskType::Key;
        KeyTask.Key = &Key;
        Stack.Add(MoveTemp(KeyTask));
      }
      break;
    }
    case EJson::Null:
    case EJson::None:
    default:
      Out += TEXT("null");
      break;
    }
  }
}

FString StableJsonKey(const TSharedPtr<FJsonValue> &Value) {
  FString Result;
  AppendStableJson(Value, Result);
  return Result;
}

// Equality does not need two complete serialized trees. This consumes the
// same exact scalar material and object keys as AppendStableJson. Unlike the
// old FString operator==, comparison is explicitly case-sensitive, so a
// case-only authoring change is not lost. Native FName semantics are unchanged.
// Malformed
// containers/cycles retain the legacy key path instead of inventing equality.
bool SameStableJson(const TSharedPtr<FJsonValue>& Left, const TSharedPtr<FJsonValue>& Right) {
  if (Left == Right) return true;
  struct FPair {
    const FJsonValue* A;
    const FJsonValue* B;
    bool bExit = false;
  };
  TArray<FPair, TInlineAllocator<32>> Pending;
  TSet<const void*> ActiveA, ActiveB;
  Pending.Add({Left.Get(), Right.Get()});
  const auto Legacy = [&]() { return StableJsonKey(Left).Equals(StableJsonKey(Right), ESearchCase::CaseSensitive); };
  const auto NumberKey = [](const FJsonValue& Value) {
    FString Lexeme, Normalized;
    if (!UE::UAI::StateSemantic::CanonicalJson::TryGetNumberLexeme(Value, Lexeme)
        || !UE::UAI::StateSemantic::TryNormalizeNumberLexeme(Lexeme, Normalized)) return FString(TEXT("invalid-number"));
    double Number = 0.0;
    if (Value.TryGetNumber(Number) && FMath::IsFinite(Number)) {
      const FString LegacyNumber = FString::Printf(TEXT("%.17g"), Number == 0.0 ? 0.0 : Number);
      FString LegacyNormalized;
      if (UE::UAI::StateSemantic::TryNormalizeNumberLexeme(LegacyNumber, LegacyNormalized)
          && LegacyNormalized == Normalized) Normalized = LegacyNumber;
    }
    return FString(TEXT("n")) + Normalized;
  };
  while (!Pending.IsEmpty()) {
    const FPair Pair = Pending.Pop(EAllowShrinking::No);
    const FJsonValue* A = Pair.A;
    const FJsonValue* B = Pair.B;
    if (Pair.bExit) {
      ActiveA.Remove(A->Type == EJson::Object ? static_cast<const void*>(A->AsObject().Get()) : &A->AsArray());
      ActiveB.Remove(B->Type == EJson::Object ? static_cast<const void*>(B->AsObject().Get()) : &B->AsArray());
      continue;
    }
    if (!A || !B) { if (A != B) return false; continue; }
    if (A->Type != B->Type) {
      if ((A->Type == EJson::None || A->Type == EJson::Null)
          && (B->Type == EJson::None || B->Type == EJson::Null)) continue;
      // A malformed container can emit the same legacy invalid-cycle token
      // as a different container kind. Keep that unusual behavior intact.
      if (A->Type == EJson::Array || A->Type == EJson::Object
          || B->Type == EJson::Array || B->Type == EJson::Object) return Legacy();
      return false;
    }
    switch (A->Type) {
    case EJson::String:
      if (!A->AsString().Equals(B->AsString(), ESearchCase::CaseSensitive)) return false;
      break;
    case EJson::Number:
      if (NumberKey(*A) != NumberKey(*B)) return false;
      break;
    case EJson::Boolean:
      if (A->AsBool() != B->AsBool()) return false;
      break;
    case EJson::Array: {
      const auto& X = A->AsArray(); const auto& Y = B->AsArray();
      if (ActiveA.Contains(&X) || ActiveB.Contains(&Y)) return Legacy();
      if (X.Num() != Y.Num()) return false;
      ActiveA.Add(&X); ActiveB.Add(&Y); Pending.Add({A, B, true});
      for (int32 I = X.Num() - 1; I >= 0; --I) Pending.Add({X[I].Get(), Y[I].Get()});
      break;
    }
    case EJson::Object: {
      const auto* X = A->AsObject().Get(); const auto* Y = B->AsObject().Get();
      if (!X || !Y || ActiveA.Contains(X) || ActiveB.Contains(Y)) return Legacy();
      if (X->Values.Num() != Y->Values.Num()) return false;
      ActiveA.Add(X); ActiveB.Add(Y); Pending.Add({A, B, true});
      for (const auto& Field : X->Values) {
        const auto Id = Y->Values.FindId(Field.Key);
        if (!Id.IsValidId()) return false;
        const auto& Other = Y->Values.Get(Id);
        if (!Field.Key.Equals(Other.Key, ESearchCase::CaseSensitive)) return false;
        Pending.Add({Field.Value.Get(), Other.Value.Get()});
      }
      break;
    }
    default: break;
    }
  }
  return true;
}

TSharedPtr<FJsonValue>
NormalizePropertyValue(FProperty *Property, const TSharedPtr<FJsonValue> &Value,
                       bool bIgnoreFixedArray = false,
                       TSharedPtr<FJsonValue>* InOutDefinition = nullptr) {
  if (InOutDefinition) *InOutDefinition = CloneJsonValue(*InOutDefinition);
  const auto Definition = InOutDefinition && *InOutDefinition && (*InOutDefinition)->Type == EJson::Object
      ? (*InOutDefinition)->AsObject() : nullptr;
  const TSharedPtr<FJsonObject>* FieldType = nullptr;
  if (Definition && Definition->TryGetObjectField(TEXT("type"), FieldType)) {
    auto NestedDefinition = Definition->TryGetField(TEXT("value_definition"));
    auto Result = NormalizePropertyValue(Property, Value, bIgnoreFixedArray, &NestedDefinition);
    if (NestedDefinition) Definition->SetField(TEXT("value_definition"), NestedDefinition);
    return Result;
  }
  auto NormalizeField = [&](FProperty* Field, const TSharedPtr<FJsonValue>& FieldValue,
                            const TSharedPtr<FJsonObject>& Fields, const FString& Name) {
    auto FieldDefinition = Fields ? Fields->TryGetField(Name) : nullptr;
    auto Result = NormalizePropertyValue(Field, FieldValue, false, &FieldDefinition);
    if (Fields && FieldDefinition) Fields->SetField(Name, FieldDefinition);
    return Result;
  };
  auto DefinitionFields = [](const TSharedPtr<FJsonObject>& Object) -> TSharedPtr<FJsonObject> {
    const TSharedPtr<FJsonObject>* Fields = nullptr;
    return Object && Object->TryGetObjectField(TEXT("fields"), Fields) ? *Fields : nullptr;
  };
  struct FItem {
    TSharedPtr<FJsonValue> Value;
    TSharedPtr<FJsonValue> Definition;
  };
  auto NormalizeItems = [&](FProperty* Element, const TArray<TSharedPtr<FJsonValue>>& Source,
                            const TSharedPtr<FJsonObject>& ItemSchema, bool bFixed, bool bSort) {
    const TArray<TSharedPtr<FJsonValue>>* Definitions = nullptr;
    if (ItemSchema) ItemSchema->TryGetArrayField(TEXT("items"), Definitions);
    TArray<FItem> Paired;
    for (int32 Index = 0; Index < Source.Num(); ++Index) {
      FItem Item;
      Item.Definition = Definitions && Definitions->IsValidIndex(Index) ? (*Definitions)[Index] : nullptr;
      Item.Value = NormalizePropertyValue(Element, Source[Index], bFixed, &Item.Definition);
      Paired.Add(MoveTemp(Item));
    }
    if (bSort) Paired.Sort([](const FItem& Left, const FItem& Right) { return StableJsonKey(Left.Value) < StableJsonKey(Right.Value); });
    TArray<TSharedPtr<FJsonValue>> Values, Schemas;
    for (const auto& Item : Paired) {
      Values.Add(Item.Value);
      Schemas.Add(Item.Definition ? Item.Definition : MakeShared<FJsonValueNull>());
    }
    if (Definitions) ItemSchema->SetArrayField(TEXT("items"), Schemas);
    return Values;
  };
  if (!Property || !Value.IsValid())
    return CloneJsonValue(Value);
  if (Property->ArrayDim > 1 && !bIgnoreFixedArray &&
      Value->Type == EJson::Object) {
    TSharedPtr<FJsonObject> Object = CloneJsonObject(Value->AsObject());
    const TArray<TSharedPtr<FJsonValue>> *SourceItems = nullptr;
    if (Object->TryGetArrayField(TEXT("items"), SourceItems) && SourceItems) {
      Object->SetArrayField(TEXT("items"), NormalizeItems(Property, *SourceItems, Definition, true, false));
    }
    return MakeShared<FJsonValueObject>(Object);
  }

  if (FStructProperty *Struct = CastField<FStructProperty>(Property)) {
    if (UeAgentAuthoringTypeDescriptors::GetStructKind(Struct->Struct) !=
            TEXT("struct") ||
        Value->Type != EJson::Object) {
      return CloneJsonValue(Value);
    }
    TSharedPtr<FJsonObject> Object = CloneJsonObject(Value->AsObject());
    const TSharedPtr<FJsonObject> *Fields = nullptr;
    if (Object->TryGetObjectField(TEXT("fields"), Fields) && Fields &&
        Fields->IsValid()) {
      TSharedPtr<FJsonObject> NormalizedFields = CloneJsonObject(*Fields);
      for (TFieldIterator<FProperty> It(Struct->Struct,
                                        EFieldIterationFlags::IncludeSuper);
           It; ++It) {
        if (const TSharedPtr<FJsonValue> FieldValue =
                NormalizedFields->TryGetField(It->GetName())) {
          NormalizedFields->SetField(It->GetName(), NormalizeField(*It, FieldValue, DefinitionFields(Definition), It->GetName()));
        }
      }
      Object->SetObjectField(TEXT("fields"), NormalizedFields);
    }
    return MakeShared<FJsonValueObject>(Object);
  }
  if (FArrayProperty *Array = CastField<FArrayProperty>(Property)) {
    TSharedPtr<FJsonObject> Object = Value->Type == EJson::Object
                                         ? CloneJsonObject(Value->AsObject())
                                         : nullptr;
    const TArray<TSharedPtr<FJsonValue>> *SourceItems = nullptr;
    if (!Object.IsValid() ||
        !Object->TryGetArrayField(TEXT("items"), SourceItems) || !SourceItems)
      return CloneJsonValue(Value);
    Object->SetArrayField(TEXT("items"), NormalizeItems(Array->Inner, *SourceItems, Definition, false, false));
    return MakeShared<FJsonValueObject>(Object);
  }
  if (FSetProperty *Set = CastField<FSetProperty>(Property)) {
    TSharedPtr<FJsonObject> Object = Value->Type == EJson::Object
                                         ? CloneJsonObject(Value->AsObject())
                                         : nullptr;
    const TArray<TSharedPtr<FJsonValue>> *SourceItems = nullptr;
    if (!Object.IsValid() ||
        !Object->TryGetArrayField(TEXT("items"), SourceItems) || !SourceItems)
      return CloneJsonValue(Value);
    Object->SetArrayField(TEXT("items"), NormalizeItems(Set->ElementProp, *SourceItems, Definition, false, true));
    return MakeShared<FJsonValueObject>(Object);
  }
  if (FMapProperty *Map = CastField<FMapProperty>(Property)) {
    TSharedPtr<FJsonObject> Object = Value->Type == EJson::Object
                                         ? CloneJsonObject(Value->AsObject())
                                         : nullptr;
    const TArray<TSharedPtr<FJsonValue>> *SourceEntries = nullptr;
    if (!Object.IsValid() ||
        !Object->TryGetArrayField(TEXT("entries"), SourceEntries) ||
        !SourceEntries)
      return CloneJsonValue(Value);
    const auto Fields = DefinitionFields(Definition);
    const TSharedPtr<FJsonObject>* EntriesSchemaPtr = nullptr;
    const auto EntriesSchema = Fields && Fields->TryGetObjectField(TEXT("entries"), EntriesSchemaPtr) ? *EntriesSchemaPtr : nullptr;
    const TArray<TSharedPtr<FJsonValue>>* EntryDefinitions = nullptr;
    if (EntriesSchema) EntriesSchema->TryGetArrayField(TEXT("items"), EntryDefinitions);
    TArray<FItem> Entries;
    for (int32 Index = 0; Index < SourceEntries->Num(); ++Index) {
      const auto& SourceEntryValue = (*SourceEntries)[Index];
      FItem Item;
      Item.Definition = EntryDefinitions && EntryDefinitions->IsValidIndex(Index) ? CloneJsonValue((*EntryDefinitions)[Index]) : nullptr;
      if (!SourceEntryValue.IsValid() ||
          SourceEntryValue->Type != EJson::Object) {
        Item.Value = CloneJsonValue(SourceEntryValue);
        Entries.Add(MoveTemp(Item));
        continue;
      }
      TSharedPtr<FJsonObject> Entry =
          CloneJsonObject(SourceEntryValue->AsObject());
      const auto EntryFields = DefinitionFields(Item.Definition && Item.Definition->Type == EJson::Object ? Item.Definition->AsObject() : nullptr);
      Entry->SetField(TEXT("key"),
                      NormalizeField(Map->KeyProp, Entry->TryGetField(TEXT("key")), EntryFields, TEXT("key")));
      Entry->SetField(TEXT("value"),
                      NormalizeField(Map->ValueProp, Entry->TryGetField(TEXT("value")), EntryFields, TEXT("value")));
      Item.Value = MakeShared<FJsonValueObject>(Entry);
      Entries.Add(MoveTemp(Item));
    }
    Entries.Sort([](const FItem& LeftItem, const FItem& RightItem) {
      const auto& Left = LeftItem.Value;
      const auto& Right = RightItem.Value;
      const TSharedPtr<FJsonObject> LeftObject =
          Left.IsValid() ? Left->AsObject() : nullptr;
      const TSharedPtr<FJsonObject> RightObject =
          Right.IsValid() ? Right->AsObject() : nullptr;
      const FString LeftKey = StableJsonKey(
          LeftObject.IsValid() ? LeftObject->TryGetField(TEXT("key"))
                               : nullptr);
      const FString RightKey = StableJsonKey(
          RightObject.IsValid() ? RightObject->TryGetField(TEXT("key"))
                                : nullptr);
      if (LeftKey != RightKey)
        return LeftKey < RightKey;
      return StableJsonKey(Left) < StableJsonKey(Right);
    });
    TArray<TSharedPtr<FJsonValue>> Values, Schemas;
    for (const auto& Item : Entries) {
      Values.Add(Item.Value);
      Schemas.Add(Item.Definition ? Item.Definition : MakeShared<FJsonValueNull>());
    }
    Object->SetArrayField(TEXT("entries"), Values);
    if (EntryDefinitions) EntriesSchema->SetArrayField(TEXT("items"), Schemas);
    return MakeShared<FJsonValueObject>(Object);
  }
  if (FOptionalProperty *Optional = CastField<FOptionalProperty>(Property)) {
    TSharedPtr<FJsonObject> Object = Value->Type == EJson::Object
                                         ? CloneJsonObject(Value->AsObject())
                                         : nullptr;
    if (!Object.IsValid())
      return CloneJsonValue(Value);
    bool bSet = false;
    if (Object->TryGetBoolField(TEXT("is_set"), bSet) && bSet) {
      Object->SetField(TEXT("value"), NormalizeField(Optional->GetValueProperty(),
          Object->TryGetField(TEXT("value")), DefinitionFields(Definition), TEXT("value")));
    }
    return MakeShared<FJsonValueObject>(Object);
  }
  return CloneJsonValue(Value);
}

bool IsObjectWithin(const UObject *Object, const UObject *Root) {
  if (!Object || !Root)
    return false;
  for (const UObject *Current = Object; Current;
       Current = Current->GetOuter()) {
    if (Current == Root)
      return true;
  }
  return false;
}

// Encode has already produced a fresh reliable tree. Normalization changes
// order only for map/set members; the other branches recursively clone it.
// This examines the current property shape, not a cached class or value, and
// never skips encoding, instance support or reference observation.
bool NeedsPropertyOrderNormalization(FProperty* Property, TSet<const UStruct*>& Active) {
  if (!Property) return true;
  if (CastField<FMapProperty>(Property) || CastField<FSetProperty>(Property)) return true;
  if (auto* Array = CastField<FArrayProperty>(Property))
    return NeedsPropertyOrderNormalization(Array->Inner, Active);
  if (auto* Optional = CastField<FOptionalProperty>(Property))
    return NeedsPropertyOrderNormalization(Optional->GetValueProperty(), Active);
  if (auto* Struct = CastField<FStructProperty>(Property)) {
    if (!Struct->Struct) return true;
    if (UeAgentAuthoringTypeDescriptors::GetStructKind(Struct->Struct) != TEXT("struct")) return false;
    if (Active.Contains(Struct->Struct)) return true;
    Active.Add(Struct->Struct);
    bool bRequired = false;
    for (TFieldIterator<FProperty> It(Struct->Struct, EFieldIterationFlags::IncludeSuper); It; ++It)
      if (NeedsPropertyOrderNormalization(*It, Active)) { bRequired = true; break; }
    Active.Remove(Struct->Struct);
    return bRequired;
  }
  // Owned-object fields are encoded and normalized in the existing owned
  // accessor branch. The outer object-reference normalizer only clones.
  return false;
}

bool EncodeAuthoringPropertyValue(FProperty *Property, const void *ValuePtr,
                                  UObject *Owner, const FReadLimits &Limits,
                                  int32 Depth,
                                  TSet<const UObject *> &ObjectStack,
                                  TSharedPtr<FJsonValue> &OutValue,
                                  FDiagnostic& OutError,
                                  TSharedPtr<FJsonValue> &OutValueDefinition,
                                  UE::UAI::StateSemantic::FUeAgentCanonicalValuePtr* OutDirectLiteral = nullptr) {
  OutValue.Reset();
  OutValueDefinition.Reset();
  if (OutDirectLiteral) OutDirectLiteral->Reset();
  if (!Property || !ValuePtr) {
    OutError = MissingStateField(TEXT("state_property_value_invalid"), !Property ? TEXT("The property descriptor is missing.") : TEXT("The native property value storage is missing."), !Property ? TEXT("property") : Property->GetName());
    return false;
  }
  OutError.Reset();

  UeAgentPropertyValueCodec::FOptions CodecOptions;
  using EDestination = UeAgentPropertyValueCodec::EAuthoringValueDestination;
  EDestination Destination = EDestination::None; int32 ComponentNumbers = 0;
  const bool bDirectCandidate = OutDirectLiteral && Depth == 0;
  if (bDirectCandidate) {
    CodecOptions.DirectAuthoringLiteral = OutDirectLiteral;
    CodecOptions.DirectAuthoringLiteralError = &OutError;
    CodecOptions.DirectAuthoringDestination = &Destination;
    if (ActiveCaptureTiming) CodecOptions.DirectComponentNumbers = &ComponentNumbers;
  }
  CodecOptions.bCaptureOwnedObjectDefinitions = true;
  CodecOptions.bCaptureStructFieldDescriptors = true;
  CodecOptions.bShareImmutableFieldDefinitions = true;
  // This builtin immutable consumer never calls BuildDefinition. Its actual
  // public value/schema and field-emission projection are separate outputs.
  CodecOptions.bCaptureTypeProvenance = OutDirectLiteral == nullptr;
  if (OutDirectLiteral && ActiveCaptureTiming) CodecOptions.RetiredTypeRecords = &ActiveCaptureTiming->RetiredTypeRecords;
  CodecOptions.StructFieldFilter = Limits.StructFieldFilter;
  if (Limits.AuthorizeProperty) CodecOptions.StructFieldAuthorization = &Limits.AuthorizeProperty;
  if (Limits.bApplyAuthoringStructFieldPolicy) {
    // Keep the original editable/authorize/filter order, but only create the
    // codec-local adapter here. Stored limits retain the actual caller policy.
    CodecOptions.StructFieldFilter = [&Limits](const UScriptStruct* Struct, const FProperty* Field) {
      return (UeAgentPropertyValueCodec::IsStructFieldAuthoringEditable(Field, Struct)
          || (Limits.AuthorizeProperty && Limits.AuthorizeProperty(Field)))
          && (!Limits.StructFieldFilter || Limits.StructFieldFilter(Struct, Field));
    };
  }
  CodecOptions.ObjectReferenceEncoder =
      [&CodecOptions, &Limits, &ObjectStack, &OutError](
          const FObjectPropertyBase *ObjectProperty, const void *ObjectValuePtr,
          UObject *ObjectOwner, int32 ObjectDepth,
          TSharedPtr<FJsonValue> &OutEncoded, TSharedPtr<FJsonObject>* OutDefinition) -> bool {
    if (!UeAgentAuthoringReflection::IsOwnedObjectProperty(ObjectProperty)
        && !UeAgentAuthoringReflection::IsExistingInlineObjectProperty(
            ObjectProperty, ObjectValuePtr, ObjectOwner))
      return false;
    auto FailOwnedObject = [&OutEncoded, &OutError](FDiagnostic Error) {
      OutError = MoveTemp(Error);
      OutEncoded = MakeShared<FJsonValueNull>();
      return true;
    };

    UObject *Referenced =
        ObjectProperty->GetObjectPropertyValue(ObjectValuePtr);
    if (!Referenced) {
      if (OutDefinition) {
        *OutDefinition = MakeShared<FJsonObject>();
        (*OutDefinition)->SetStringField(TEXT("builtin"), TEXT("owned_object"));
      }
      TSharedRef<FJsonObject> NullReference = MakeShared<FJsonObject>();
      NullReference->SetField(TEXT("path"), MakeShared<FJsonValueNull>());
      OutEncoded = MakeShared<FJsonValueObject>(NullReference);
      return true;
    }
    if (!ObjectOwner || !IsObjectWithin(Referenced, ObjectOwner)) {
      auto Failure = StateFailure(TEXT("state_owned_object_outside_owner"),
          !ObjectOwner ? TEXT("The owned object reference has no containing owner.")
              : TEXT("The referenced object is outside the containing owner."),
          ObjectProperty->GetName(), MakeShared<FJsonValueString>(ObjectOwner ? ObjectOwner->GetName() : TEXT("containing owner")),
          MakeShared<FJsonValueString>(Referenced->GetName()));
      Failure.InternalContext = Referenced->GetPathName();
      if (ObjectOwner) Failure.ExpectedEvidence = MakeShared<FJsonValueString>(ObjectOwner->GetPathName());
      return FailOwnedObject(MoveTemp(Failure));
    }
    if (ObjectStack.Contains(Referenced)) {
      auto Failure = StateFailure(TEXT("state_owned_object_cycle"),
          TEXT("The owned object reference revisits an object already on the current traversal path."),
          ObjectProperty->GetName(), MakeShared<FJsonValueString>(TEXT("object outside the active traversal path")),
          MakeShared<FJsonValueString>(Referenced->GetName()));
      Failure.InternalContext = Referenced->GetPathName();
      return FailOwnedObject(MoveTemp(Failure));
    }

    TArray<FProperty *> Properties;
    TMap<FProperty*, TArray<FString>> PropertyCategories;
    for (TFieldIterator<FProperty> It(Referenced->GetClass(),
                                      EFieldIterationFlags::IncludeSuper);
         It; ++It) {
      FProperty *NestedProperty = *It;
      const void *NestedValuePtr =
          NestedProperty
              ? NestedProperty->ContainerPtrToValuePtr<void>(Referenced)
              : nullptr;
      UeAgentAuthoringReflection::FDescriptor Reflection;
      FDiagnostic ReflectionError;
      if (!UeAgentAuthoringReflection::Build(
              NestedProperty, Referenced, NestedValuePtr, Reflection, ReflectionError, Limits.bShowDefaultsOnly, Limits.bUseInstanceEditability)) {
        return FailOwnedObject(MoveTemp(ReflectionError));
      }
      if (Reflection.bEditable || (Limits.AuthorizeProperty && Limits.AuthorizeProperty(NestedProperty))) {
        Properties.Add(NestedProperty);
        PropertyCategories.Add(NestedProperty, Reflection.CategoryPath);
      }
    }
    Properties.Sort([](const FProperty &Left, const FProperty &Right) {
      return Left.GetName() < Right.GetName();
    });

    ObjectStack.Add(Referenced);
    TArray<TSharedPtr<FJsonValue>> PropertyValues;
    TArray<TSharedPtr<FJsonValue>> PropertyDefinitions;
    PropertyValues.Reserve(Properties.Num());
    for (FProperty *NestedProperty : Properties) {
      TSharedPtr<FJsonObject> NestedDefinition;
      TSharedPtr<FJsonValue> NestedValue = UeAgentPropertyValueCodec::Encode(
          NestedProperty,
          NestedProperty->ContainerPtrToValuePtr<void>(Referenced), Referenced,
          CodecOptions, ObjectDepth + 1, OutDefinition ? &NestedDefinition : nullptr);
      if (!OutError.IsEmpty()) {
        ObjectStack.Remove(Referenced);
        OutEncoded = MakeShared<FJsonValueNull>();
        return true;
      }
      FDiagnostic ReliabilityReason;
      if (!UeAgentPropertyValueCodec::IsReliableCanonicalValue(
              NestedProperty, NestedValue, &ReliabilityReason)) {
        ObjectStack.Remove(Referenced);
        if (ReliabilityReason.PropertyPath.IsEmpty()) ReliabilityReason.PropertyPath = NestedProperty->GetName();
        return FailOwnedObject(MoveTemp(ReliabilityReason));
      }
      TSharedRef<FJsonObject> Descriptor = MakeShared<FJsonObject>();
      Descriptor->SetStringField(TEXT("property_path"),
                                 NestedProperty->GetName());
      TSharedPtr<FJsonValue> NormalizedDefinition = NestedDefinition
          ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueObject>(NestedDefinition)) : nullptr;
      Descriptor->SetField(TEXT("value"),
                           NormalizePropertyValue(NestedProperty, NestedValue, false, &NormalizedDefinition));
      NestedDefinition = NormalizedDefinition && NormalizedDefinition->Type == EJson::Object ? NormalizedDefinition->AsObject() : nullptr;
      PropertyValues.Add(MakeShared<FJsonValueObject>(Descriptor));
      auto DescriptorDefinition = MakeShared<FJsonObject>();
      DescriptorDefinition->SetStringField(TEXT("property_name"), NestedProperty->GetName());
      TArray<TSharedPtr<FJsonValue>> CategoryPath;
      for (const auto& Segment : PropertyCategories.FindChecked(NestedProperty))
        CategoryPath.Add(MakeShared<FJsonValueString>(Segment));
      DescriptorDefinition->SetArrayField(TEXT("category_path"), CategoryPath);
      auto PropertyDefinition = MakeShared<FJsonObject>();
      PropertyDefinition->SetObjectField(TEXT("type"), UeAgentPropertyValueCodec::BuildTypeDescriptor(NestedProperty));
      if (NestedDefinition) PropertyDefinition->SetObjectField(TEXT("value_definition"), NestedDefinition);
      if (const auto Candidates = UeAgentAuthoringTypeDescriptors::BuildEnumCandidates(
              UeAgentAuthoringTypeDescriptors::GetPropertyEnum(NestedProperty), false, NestedProperty))
        for (const auto& Field : Candidates->Values) PropertyDefinition->SetField(Field.Key, Field.Value);
      auto Fields = MakeShared<FJsonObject>();
      Fields->SetObjectField(TEXT("value"), PropertyDefinition);
      DescriptorDefinition->SetObjectField(TEXT("fields"), Fields);
      PropertyDefinitions.Add(MakeShared<FJsonValueObject>(DescriptorDefinition));
    }
    ObjectStack.Remove(Referenced);

    if (OutDefinition) {
      *OutDefinition = MakeShared<FJsonObject>();
      (*OutDefinition)->SetStringField(TEXT("builtin"), TEXT("owned_object"));
      // Edit binding metadata is relative to the native value owner, not the
      // proposed scratch package. It does not change business-value equality.
      (*OutDefinition)->SetStringField(TEXT("owner_relative_path"), Referenced->GetPathName(ObjectOwner));
      (*OutDefinition)->SetStringField(TEXT("instance_name"), Referenced->GetClass()->GetDisplayNameText().ToString());
      const bool bExistingInlineOnly =
          UeAgentAuthoringReflection::IsExistingInlineObjectProperty(ObjectProperty, ObjectValuePtr, ObjectOwner)
          && (!UeAgentAuthoringReflection::IsOwnedObjectProperty(ObjectProperty)
              || (!UeAgentAuthoringReflection::IsEditableByFlags(ObjectProperty->GetOwnerProperty(), ObjectOwner, Limits.bShowDefaultsOnly, Limits.bUseInstanceEditability)
                  && !(Limits.AuthorizeProperty && Limits.AuthorizeProperty(ObjectProperty->GetOwnerProperty()))));
      if (bExistingInlineOnly)
        (*OutDefinition)->SetBoolField(TEXT("existing_inline_only"), true);
      auto PropertiesDefinition = MakeShared<FJsonObject>();
      PropertiesDefinition->SetStringField(TEXT("element_kind"), TEXT("reflected_property"));
      PropertiesDefinition->SetArrayField(TEXT("items"), PropertyDefinitions);
      auto OwnedFields = MakeShared<FJsonObject>();
      OwnedFields->SetObjectField(TEXT("properties"), PropertiesDefinition);
      auto OwnedDefinition = MakeShared<FJsonObject>();
      // This is the reflected-property portion of a native owned record. Class,
      // selection and identity metadata are validated by the owned-value owner.
      OwnedDefinition->SetBoolField(TEXT("fields_complete"), false);
      OwnedDefinition->SetObjectField(TEXT("fields"), OwnedFields);
      auto Fields = MakeShared<FJsonObject>();
      Fields->SetObjectField(TEXT("owned_object"), OwnedDefinition);
      (*OutDefinition)->SetObjectField(TEXT("fields"), Fields);
    }

    TSharedRef<FJsonObject> OwnedObject = MakeShared<FJsonObject>();
    OwnedObject->SetStringField(TEXT("class_path"),
                                Referenced->GetClass()->GetPathName());
    OwnedObject->SetArrayField(TEXT("properties"), PropertyValues);
    TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
    Value->SetObjectField(TEXT("owned_object"), OwnedObject);
    OutEncoded = MakeShared<FJsonValueObject>(Value);
    return true;
  };

  TSharedPtr<FJsonObject> Definition;
  TSharedPtr<FJsonValue> Encoded = UeAgentPropertyValueCodec::Encode(
      Property, ValuePtr, Owner, CodecOptions, Depth, &Definition);
  if (!OutError.IsEmpty())
    return false;
  const bool bDirectValue = Destination == EDestination::Scalar || Destination == EDestination::BuiltinShape
      || Destination == EDestination::Reference || Destination == EDestination::Sequence || Destination == EDestination::Bitmask;
  if (bDirectCandidate && Destination == EDestination::None) {
    OutError = MissingStateField(TEXT("state_direct_literal_handoff_invalid"),
        TEXT("The selected codec did not report its actual output destination; no other encoder was selected."), TEXT("value")); return false;
  }
  if (bDirectCandidate && !bDirectValue && OutDirectLiteral->IsValid()) {
    OutError = MissingStateField(TEXT("state_direct_literal_handoff_invalid"),
        TEXT("The codec reported an original JSON input and an independent immutable value."), TEXT("value")); return false;
  }
  if (bDirectValue) {
    const bool bDeclaredBitmask = Destination == EDestination::Bitmask;
    if (!OutDirectLiteral->IsValid() || Encoded || (Definition.IsValid() != bDeclaredBitmask)) {
      OutError = MissingStateField(TEXT("state_direct_literal_handoff_invalid"),
          TEXT("The selected builtin literal codec did not supply exactly its immutable value; no JSON retry was selected."), TEXT("value"));
      return false;
    }
    // The selected names/bits are a pure value, while the original mask/width
    // contract remains a real definition/comparison output of this same codec.
    if (bDeclaredBitmask) OutValueDefinition = MakeShared<FJsonValueObject>(MoveTemp(Definition));
    if (ActiveCaptureTiming) {
      if (Destination == EDestination::BuiltinShape) {
        ++ActiveCaptureTiming->DirectClosedShapes;
        ActiveCaptureTiming->DirectShapeNumbers += ComponentNumbers;
      } else if (Destination == EDestination::Reference) ++ActiveCaptureTiming->DirectReferences;
      else if (Destination == EDestination::Sequence) ++ActiveCaptureTiming->DirectSequences;
      else if (Destination == EDestination::Bitmask) ++ActiveCaptureTiming->DirectBitmasks;
      else {
        ++ActiveCaptureTiming->DirectLiterals;
        if ((*OutDirectLiteral)->GetObjectItems()[0].Key == TEXT("selected")) ++ActiveCaptureTiming->DirectEnums;
        else if (CastField<FInt64Property>(Property) || CastField<FUInt64Property>(Property)) ++ActiveCaptureTiming->DirectIntegerStrings;
        else if (CastField<FNumericProperty>(Property)) ++ActiveCaptureTiming->DirectNumbers;
      }
    }
    return true;
  }
  FDiagnostic ReliabilityReason;
  if (!UeAgentPropertyValueCodec::IsReliableCanonicalValue(
          Property, Encoded, &ReliabilityReason)) {
    OutError = MoveTemp(ReliabilityReason);
    if (OutError.PropertyPath.IsEmpty()) OutError.PropertyPath = Property->GetName();
    return false;
  }
  if (Definition.IsValid()) OutValueDefinition = MakeShared<FJsonValueObject>(Definition);
  TSet<const UStruct*> NormalizationTypes;
  const bool bNormalizeOrder = NeedsPropertyOrderNormalization(Property, NormalizationTypes);
  if (ActiveCaptureTiming) {
    if (bNormalizeOrder) ++ActiveCaptureTiming->OrderedNormalizations;
    else ++ActiveCaptureTiming->FreshEncodedTransfers;
  }
  OutValue = bNormalizeOrder
      ? NormalizePropertyValue(Property, Encoded, false, &OutValueDefinition) : MoveTemp(Encoded);
  OutError.Reset();
  return true;
}

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentNativeValueDestinationTest,
    "UeAgentInterface.Core.AuthoringState.NativeValueDestination",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentNativeValueDestinationTest::RunTest(const FString&) {
  // Local native storage only. TFunction's real copy contract is observable;
  // the value must be read by the codec after its original option copies.
  double Native = 1; bool bArmed = false; int32 Copies = 0;
  struct FFilter {
    double* Native; bool* Armed; int32* Copies;
    FFilter(double* N, bool* A, int32* C) : Native(N), Armed(A), Copies(C) {}
    FFilter(const FFilter& Other) : Native(Other.Native), Armed(Other.Armed), Copies(Other.Copies) {
      if (*Armed) { *Native = 2; ++*Copies; }
    }
    bool operator()(const UScriptStruct*, const FProperty*) const { return true; }
  };
  FReadLimits Limits; Limits.StructFieldFilter = FFilter(&Native, &bArmed, &Copies);
  auto* Property = FindFProperty<FDoubleProperty>(TBaseStructure<FVector>::Get(), TEXT("X"));
  if (!TestNotNull(TEXT("actual native float64 descriptor"), Property)) return false;
  TSet<const UObject*> Stack; TSharedPtr<FJsonValue> Original, Definition, DirectJson;
  FDiagnostic Error; UE::UAI::StateSemantic::FUeAgentCanonicalValuePtr Direct;
  UeAgentAuthoringEncodedTypes::FScope EncodedTypes;
  bArmed = true;
  if (!TestTrue(TEXT("original State codec input succeeds"),
      EncodeAuthoringPropertyValue(Property, &Native, nullptr, Limits, 0, Stack, Original, Error, Definition))) return false;
  const int32 OriginalCopies = Copies;
  TestTrue(TEXT("original option copies actually ran before read"), OriginalCopies >= 2);
  TestEqual(TEXT("original native observation sees callback-copy effect"), Original->AsObject()->GetNumberField(TEXT("literal")), 2.0);
  Native = 1; Copies = 0;
  if (!TestTrue(TEXT("immutable State destination succeeds"),
      EncodeAuthoringPropertyValue(Property, &Native, nullptr, Limits, 0, Stack, DirectJson, Error, Definition, &Direct))) return false;
  TestEqual(TEXT("same original option-copy sequence"), Copies, OriginalCopies);
  TestTrue(TEXT("only immutable value is produced"), Direct && !DirectJson && !Definition);
  TestTrue(TEXT("immutable consumer observes native value at original read point"),
      Direct && AreStableCanonicalValuesEqual(Original, UeAgentPropertyValueCodec::ProjectDirectAuthoringValue(*Direct)));
  bArmed = false;
  // The actual reflection producer owns the pure declaration once. Its
  // complete DTO purpose and the export view must expose identical facts.
  UeAgentAuthoringReflection::FScopedDescriptorMetadata Metadata;
  UeAgentAuthoringReflection::FDescriptor Owning, Borrowed;
  const auto* Scope = Property->GetOwnerStruct();
  if (!TestTrue(TEXT("original authoring descriptor"), UeAgentAuthoringReflection::BuildAuthoring(
      Property, nullptr, &Native, Owning, Error, false, false, Scope))
      || !TestTrue(TEXT("same-owner shared declaration"), UeAgentAuthoringReflection::BuildAuthoringSharedDeclaration(
      Property, nullptr, &Native, Borrowed, Error, false, false, Scope))) return false;
  TestTrue(TEXT("export does not copy the declaration DTO"), Borrowed.SharedDeclaration && Borrowed.Type.Kind.IsEmpty() && Borrowed.Constraints.IsEmpty());
  TestTrue(TEXT("complete original type is still available"), AreStableCanonicalValuesEqual(
      MakeShared<FJsonValueObject>(Owning.Type.ToJsonObject()), MakeShared<FJsonValueObject>(Borrowed.GetType().ToJsonObject())));
  TestTrue(TEXT("same original category output without per-instance copy"), Borrowed.SharedCategoryPath
      && Borrowed.CategoryPath.IsEmpty() && AreStableCanonicalValuesEqual(
          MakeShared<FJsonValueArray>(Owning.MakeCategoryPathJson()), MakeShared<FJsonValueArray>(Borrowed.MakeCategoryPathJson())));
  const auto OwningConstraints = Owning.MakeConstraintsJson(), BorrowedConstraints = Borrowed.MakeConstraintsJson();
  TestTrue(TEXT("original constraints output remains identical"), (!OwningConstraints && !BorrowedConstraints)
      || (OwningConstraints && BorrowedConstraints && AreStableCanonicalValuesEqual(
          MakeShared<FJsonValueObject>(OwningConstraints), MakeShared<FJsonValueObject>(BorrowedConstraints))));
  // Original public capture/read APIs, local Struct memory only. No UObject
  // mutation, fixture/asset, alternate codec or Snapshot test backdoor.
  FVector Local(3, 4, 5); FReadRequest LocalRequest;
  UeAgentAuthoringTargetLocator::FResolveResult Resolved;
  if (!TestTrue(TEXT("original struct request builder"), BuildStructPropertyReadRequest(TBaseStructure<FVector>::Get(),
      &Local, TBaseStructure<FVector>::Get()->GetPathName(), nullptr, TEXT("X"), TEXT("core.property"), LocalRequest, Resolved))) {
    AddError(Resolved.Diagnostic.Code); return false;
  }
  TArray<FReadRequest> Requests{MoveTemp(LocalRequest)}; FCaptureResult DefaultCapture, ExportCapture;
  if (!TestTrue(TEXT("default snapshot consumer"), CaptureSnapshot(Requests, DefaultCapture))
      || !TestTrue(TEXT("explicit export snapshot consumer"), CaptureSnapshot(Requests, ExportCapture, nullptr, nullptr,
          ECaptureValueMode::AuthoringExportLiterals))) {
    AddError(DefaultCapture.Diagnostic.Code + TEXT("|") + ExportCapture.Diagnostic.Code); return false;
  }
  const auto& DefaultEntry = DefaultCapture.Snapshot.ReadAuthoringEntry(0);
  const auto& ExportEntry = ExportCapture.Snapshot.ReadAuthoringEntry(0);
  TestTrue(TEXT("default observations and native storage retained"), DefaultEntry.Observation
      && DefaultCapture.Snapshot.GetObservationMembership() && DefaultCapture.Snapshot.ReadNativeBinding(0).NativeValue);
  TestTrue(TEXT("default comparison definition remains a real consumer"), DefaultEntry.ComparisonValueDefinition.IsValid());
  auto VerificationRequests = Requests;
  VerificationRequests[0].MarkVerificationOnly();
  FCaptureResult Verification;
  if (!TestTrue(TEXT("verification-only request keeps its native capture"),
      CaptureSnapshotWithBaseline(VerificationRequests, DefaultCapture.Snapshot, Verification))) return false;
  const auto& VerifiedEntry = Verification.Snapshot.ReadAuthoringEntry(0);
  TestTrue(TEXT("verification role changes only public visibility"), DefaultEntry.bAuthoringVisible
      && !VerifiedEntry.bAuthoringVisible && Verification.Snapshot.ReadNativeBinding(0).NativeValue
      && VerifiedEntry.TargetIdentity.CanonicalKey == DefaultEntry.TargetIdentity.CanonicalKey
      && AreStableCanonicalValuesEqual(DefaultEntry.Value, VerifiedEntry.Value));
  TestTrue(TEXT("export has no unused observation/native payload"), !ExportEntry.Observation
      && !ExportCapture.Snapshot.GetObservationMembership() && ExportCapture.Snapshot.GetObservationBlocks().IsEmpty()
      && !ExportCapture.Snapshot.ReadNativeBinding(0).NativeValue);
  TestTrue(TEXT("export raw view owns its pure declaration"), ExportEntry.SharedDeclaration && ExportEntry.Type.Kind.IsEmpty());
  TestTrue(TEXT("original current value reaches export"), AreStableCanonicalValuesEqual(DefaultEntry.Value,
      UeAgentPropertyValueCodec::ProjectDirectAuthoringValue(*ExportEntry.DirectAuthoringValue)));
  TestTrue(TEXT("default entry API materializes complete type"), ExportCapture.Snapshot.ReadEntry(0).Type == ExportEntry.GetType());
  TestTrue(TEXT("legacy array API materializes complete type"), ExportCapture.Snapshot.GetEntries()[0].Type == ExportEntry.GetType());
  FStateEntry Writable;
  TestTrue(TEXT("writable copy has original independent declaration"), ExportCapture.Snapshot.CopyEntry(0, Writable)
      && !Writable.SharedDeclaration && Writable.Type == ExportEntry.GetType());
  FReadRequest OtherAxis;
  if (!TestTrue(TEXT("second local field request"), BuildStructPropertyReadRequest(TBaseStructure<FVector>::Get(),
      &Local, TBaseStructure<FVector>::Get()->GetPathName(), nullptr, TEXT("Y"), TEXT("core.property"), OtherAxis, Resolved))) return false;
  TArray<FReadRequest> UnorderedRequests{MoveTemp(OtherAxis), Requests[0]};
  FCaptureResult DefaultOrdered, ExportOrdered;
  if (!TestTrue(TEXT("default unordered local fields"), CaptureSnapshot(UnorderedRequests, DefaultOrdered))
      || !TestTrue(TEXT("export unordered local fields"), CaptureSnapshot(UnorderedRequests, ExportOrdered, nullptr, nullptr,
          ECaptureValueMode::AuthoringExportLiterals))) return false;
  if (!TestEqual(TEXT("same final ordered entry count"), ExportOrdered.Snapshot.Num(), DefaultOrdered.Snapshot.Num())) return false;
  for (int32 Index = 0; Index < DefaultOrdered.Snapshot.Num(); ++Index) {
    const auto& Before = DefaultOrdered.Snapshot.ReadAuthoringEntry(Index);
    const auto& Actual = ExportOrdered.Snapshot.ReadAuthoringEntry(Index);
    TestTrue(TEXT("same original ordering and current value"),
        Before.SemanticId == Actual.SemanticId && Before.TargetIdentity.CanonicalKey == Actual.TargetIdentity.CanonicalKey
        && AreStableCanonicalValuesEqual(Before.Value, UeAgentPropertyValueCodec::ProjectDirectAuthoringValue(*Actual.DirectAuthoringValue)));
  }
  TArray<FReadRequest> DuplicateRequests{Requests[0], Requests[0]};
  FCaptureResult DefaultDuplicate, ExportDuplicate;
  TestFalse(TEXT("default duplicate remains rejected"), CaptureSnapshot(DuplicateRequests, DefaultDuplicate));
  TestFalse(TEXT("export duplicate remains rejected"), CaptureSnapshot(DuplicateRequests, ExportDuplicate, nullptr, nullptr,
      ECaptureValueMode::AuthoringExportLiterals));
  TestTrue(TEXT("same duplicate first error and target"), DefaultDuplicate.Diagnostic.Code == ExportDuplicate.Diagnostic.Code
      && DefaultDuplicate.Diagnostic.InternalContext == ExportDuplicate.Diagnostic.InternalContext
      && DefaultDuplicate.TargetIdentity.CanonicalKey == ExportDuplicate.TargetIdentity.CanonicalKey);
  const auto* PrimitiveClass = FindObject<UClass>(nullptr, TEXT("/Script/Engine.PrimitiveComponent"));
  auto* MaskProperty = PrimitiveClass ? FindFProperty<FNumericProperty>(PrimitiveClass, TEXT("ExcludeFromHLODLevels")) : nullptr;
  if (!TestNotNull(TEXT("actual HLOD integer bitmask descriptor"), MaskProperty)) return false;
  const uint64 StorageMask = UeAgentAuthoringTypeDescriptors::GetIntegerStorageMask(MaskProperty);
  for (const uint64 Input : {uint64(0), uint64(1), StorageMask}) {
    uint64 LocalBits = 0; MaskProperty->SetIntPropertyValue(&LocalBits, Input);
    TSharedPtr<FJsonValue> JsonBits, DirectBitsJson, JsonBitsDefinition, DirectBitsDefinition;
    UE::UAI::StateSemantic::FUeAgentCanonicalValuePtr DirectBits;
    if (!TestTrue(TEXT("original bitmask value/schema"), EncodeAuthoringPropertyValue(MaskProperty, &LocalBits, nullptr,
        Limits, 0, Stack, JsonBits, Error, JsonBitsDefinition))
        || !TestTrue(TEXT("same codec direct bitmask value/schema"), EncodeAuthoringPropertyValue(MaskProperty, &LocalBits, nullptr,
            Limits, 0, Stack, DirectBitsJson, Error, DirectBitsDefinition, &DirectBits))) return false;
    TestTrue(TEXT("bitmask has one current value and the same real definition"), DirectBits && !DirectBitsJson
        && AreStableCanonicalValuesEqual(JsonBits, UeAgentPropertyValueCodec::ProjectDirectAuthoringValue(*DirectBits))
        && AreStableCanonicalValuesEqual(JsonBitsDefinition, DirectBitsDefinition));
    FStateEntry JsonEntry, DirectEntry; JsonEntry.Value = JsonBits; DirectEntry.DirectAuthoringValue = DirectBits;
    FString JsonMaterial, DirectMaterial;
    VisitStableCanonicalValueMaterialView(JsonEntry, [&](FStringView Text) { JsonMaterial.AppendChars(Text.GetData(), Text.Len()); });
    VisitStableCanonicalValueMaterialView(DirectEntry, [&](FStringView Text) { DirectMaterial.AppendChars(Text.GetData(), Text.Len()); });
    TestEqual(TEXT("same original bitmask binding grammar"), DirectMaterial, JsonMaterial);
  }
  {
    // The public projection consumes the original captured values/schema.
    // These cases exercise identity reuse and real projection, not another
    // implementation of the algorithm or an asset-side mutation.
    auto ScalarType = MakeShared<FJsonObject>(); ScalarType->SetStringField(TEXT("kind"), TEXT("bool"));
    auto StructType = MakeShared<FJsonObject>(); StructType->SetStringField(TEXT("kind"), TEXT("struct"));
    auto DescriptorA = MakeShared<FJsonObject>(), DescriptorB = MakeShared<FJsonObject>();
    for (const auto& Descriptor : {DescriptorA, DescriptorB}) {
      Descriptor->SetObjectField(TEXT("type"), ScalarType); Descriptor->SetBoolField(TEXT("editable"), true);
    }
    auto Fields = MakeShared<FJsonObject>();
    Fields->SetObjectField(TEXT("A"), DescriptorA); Fields->SetObjectField(TEXT("B"), DescriptorB);
    auto Schema = MakeShared<FJsonObject>(); Schema->SetObjectField(TEXT("fields"), Fields);
    auto Values = MakeShared<FJsonObject>();
    Values->SetBoolField(TEXT("A"), true); Values->SetBoolField(TEXT("B"), false);
    auto Object = MakeShared<FJsonObject>(); Object->SetObjectField(TEXT("fields"), Values);
    TSharedPtr<FJsonValue> Value = MakeShared<FJsonValueObject>(Object), ProjectionInputDefinition = MakeShared<FJsonValueObject>(Schema);
    TSharedPtr<FJsonValue> Projected, ProjectedDefinition;
    TestTrue(TEXT("unchanged struct stays actionable"), UeAgentPropertyValueCodec::ProjectAuthoringValue(
        StructType, Value, ProjectionInputDefinition, Projected, ProjectedDefinition));
    TestTrue(TEXT("no-change projection hands off both original objects"), Projected == Value && ProjectedDefinition == ProjectionInputDefinition);
    auto Reversed = MakeShared<FJsonObject>(); Reversed->SetBoolField(TEXT("B"), false); Reversed->SetBoolField(TEXT("A"), true);
    auto ReversedObject = MakeShared<FJsonObject>(); ReversedObject->SetObjectField(TEXT("fields"), Reversed);
    const TSharedPtr<FJsonValue> ReversedValue = MakeShared<FJsonValueObject>(ReversedObject);
    UeAgentPropertyValueCodec::ProjectAuthoringValue(StructType, ReversedValue, ProjectionInputDefinition, Projected, ProjectedDefinition);
    TestTrue(TEXT("original schema field encounter order still wins"), Projected != ReversedValue
        && Projected->AsObject()->GetObjectField(TEXT("fields"))->Values.CreateConstIterator().Key() == TEXT("A"));
    Values->SetBoolField(TEXT("Extra"), true);
    UeAgentPropertyValueCodec::ProjectAuthoringValue(StructType, Value, ProjectionInputDefinition, Projected, ProjectedDefinition);
    TestTrue(TEXT("unknown captured field is still projected out"), Projected != Value
        && !Projected->AsObject()->GetObjectField(TEXT("fields"))->HasField(TEXT("Extra")) && Values->HasField(TEXT("Extra")));
    Values->RemoveField(TEXT("Extra"));
    auto ArrayType = MakeShared<FJsonObject>(); ArrayType->SetStringField(TEXT("kind"), TEXT("array"));
    ArrayType->SetObjectField(TEXT("element_type"), StructType);
    auto ArraySchema = MakeShared<FJsonObject>(); ArraySchema->SetArrayField(TEXT("items"), {ProjectionInputDefinition});
    auto Element = MakeShared<FJsonObject>(); Element->SetObjectField(TEXT("type"), StructType); Element->SetField(TEXT("value_definition"), ProjectionInputDefinition);
    ArraySchema->SetObjectField(TEXT("element_definition"), Element); ArraySchema->SetField(TEXT("element_template"), Value);
    auto ArrayObject = MakeShared<FJsonObject>(); ArrayObject->SetArrayField(TEXT("items"), {Value});
    const TSharedPtr<FJsonValue> ArrayValue = MakeShared<FJsonValueObject>(ArrayObject), ArrayDefinition = MakeShared<FJsonValueObject>(ArraySchema);
    UeAgentPropertyValueCodec::ProjectAuthoringValue(ArrayType, ArrayValue, ArrayDefinition, Projected, ProjectedDefinition);
    TestTrue(TEXT("unchanged array/template/schema stay original"), Projected == ArrayValue && ProjectedDefinition == ArrayDefinition);
    DescriptorB->SetBoolField(TEXT("editable"), false);
    UeAgentPropertyValueCodec::ProjectAuthoringValue(ArrayType, ArrayValue, ArrayDefinition, Projected, ProjectedDefinition);
    const auto Item = Projected->AsObject()->GetArrayField(TEXT("items"))[0]->AsObject()->GetObjectField(TEXT("fields"));
    const auto Template = ProjectedDefinition->AsObject()->GetObjectField(TEXT("element_template"))->GetObjectField(TEXT("fields"));
    TestTrue(TEXT("real readonly projection reaches item and creation template"), Projected != ArrayValue
        && ProjectedDefinition != ArrayDefinition && Item->HasField(TEXT("A")) && !Item->HasField(TEXT("B"))
        && !Template->HasField(TEXT("B")) && Values->HasField(TEXT("B")));
  }
  {
    UE::UAI::ContextResource::FUeAgentResourceTracker Resources(false);
    UE::UAI::StateSemantic::FUeAgentCanonicalValuePtr One, Two;
    FDiagnostic Failure;
    if (!UE::UAI::StateSemantic::FUeAgentCanonicalValue::TryBoolean(true, Resources, One, Failure)
        || !UE::UAI::StateSemantic::FUeAgentCanonicalValue::TryBoolean(false, Resources, Two, Failure)) return false;
    FUeAgentStructuredHjsonObjectView::FEntries Entries{{TEXT("b"), One.ToSharedRef()},
        {TEXT("a/中文"), Two.ToSharedRef()}, {TEXT("b"), Two.ToSharedRef()}};
    TArray<FUeAgentStructuredHjsonObjectFieldView> OldFields;
    for (const auto& Entry : Entries) OldFields.Emplace(Entry.Key, Entry.Value.Get());
    const FUeAgentStructuredHjsonObjectView Before{MakeArrayView(OldFields)};
    FUeAgentStructuredHjsonObjectView After; After.Entries = &Entries;
    for (const bool bJsonKeys : {false, true}) {
      FUeAgentStructuredHjsonWriteOptions Options; Options.bRequireJsonCompatibleObjectKeys = bJsonKeys;
      FString OldText, NewText; FDiagnostic OldError, NewError;
      const bool bOld = FUeAgentStructuredHjsonFormatter::TryWriteValue(Before, Options, OldText, OldError);
      const bool bNew = FUeAgentStructuredHjsonFormatter::TryWriteValue(After, Options, NewText, NewError);
      TestTrue(TEXT("borrowed original pairs keep first-member/JSON-key errors and exact text"),
          bOld == bNew && OldText == NewText && OldError.Code == NewError.Code);
    }
  }
  // Real reflected array descriptor and ordinary native array storage. No Actor
  // instance is created or changed; use the original State/codec consumer twice.
  auto* ActorClass = FindObject<UClass>(nullptr, TEXT("/Script/Engine.Actor"));
  auto* NamesProperty = ActorClass ? FindFProperty<FArrayProperty>(ActorClass, TEXT("Tags")) : nullptr;
  if (!TestNotNull(TEXT("actual native Name array descriptor"), NamesProperty)) return false;
  TArray<FName> Names;
  for (int32 Pass = 0; Pass != 2; ++Pass) {
    if (Pass) { Names.Add(FName(TEXT("alpha"))); Names.Add(FName(TEXT("中文_tag"))); }
    TSharedPtr<FJsonValue> JsonArray, DirectArrayJson, ArrayDefinition;
    UE::UAI::StateSemantic::FUeAgentCanonicalValuePtr DirectArray;
    Native = 1; Copies = 0; bArmed = true;
    const bool bOriginal = EncodeAuthoringPropertyValue(NamesProperty, &Names, nullptr, Limits, 0,
        Stack, JsonArray, Error, ArrayDefinition);
    const int32 ArrayCopies = Copies;
    Copies = 0;
    const bool bDirect = EncodeAuthoringPropertyValue(NamesProperty, &Names, nullptr, Limits, 0,
        Stack, DirectArrayJson, Error, ArrayDefinition, &DirectArray);
    bArmed = false;
    TestTrue(TEXT("same original array output from immutable destination"), bOriginal && bDirect
        && DirectArray && !DirectArrayJson && !ArrayDefinition
        && AreStableCanonicalValuesEqual(JsonArray, UeAgentPropertyValueCodec::ProjectDirectAuthoringValue(*DirectArray)));
    TestEqual(TEXT("array destination adds no observable callback copies"), Copies, ArrayCopies);
  }
  {
    UeAgentAuthoringEncodedTypes::FScope PlainTypes;
    UeAgentPropertyValueCodec::FOptions Plain;
    const auto WithTypes = UeAgentPropertyValueCodec::Encode(NamesProperty, &Names, nullptr, Plain);
    TestTrue(TEXT("default codec still records its requested definition"), PlainTypes.BuildDefinition(WithTypes).IsValid());
    UeAgentAuthoringEncodedTypes::FScope ValuesOnly;
    Plain.bCaptureTypeProvenance = false;
    int64 Retired = 0; Plain.RetiredTypeRecords = &Retired;
    const auto WithoutTypes = UeAgentPropertyValueCodec::Encode(NamesProperty, &Names, nullptr, Plain);
    TestTrue(TEXT("value-only JSON is unchanged without unconsumed type maps"),
        AreStableCanonicalValuesEqual(WithTypes, WithoutTypes));
    TestTrue(TEXT("value-only sidecar production actually stopped"), Retired > 0 && !ValuesOnly.BuildDefinition(WithoutTypes));
  }
  // An actual selected object field and semantic remap exercise the native
  // identity handoff. The existing Actor CDO is read only, never created/edited.
  const auto* ActorRoot = ActorClass->GetDefaultObject(false);
  FReadRequest Bound;
  if (!TestNotNull(TEXT("existing Actor class default"), ActorRoot)
      || !TestTrue(TEXT("original object property request"), BuildPropertyReadRequest(
          ActorRoot, TEXT("Tags"), TEXT("core.property"), Bound, Resolved))) return false;
  Bound.TargetQuery = {}; Bound.SelectedRootProperty = NamesProperty; Bound.SelectedRootClass = ActorClass;
  Bound.SnapshotIdentityOverride = Bound.TargetIdentity;
  TArray<FReadRequest> BoundRequests{Bound}; FCaptureResult BoundDefault, BoundExport;
  if (!TestTrue(TEXT("default selected field"), CaptureSnapshot(BoundRequests, BoundDefault))
      || !TestTrue(TEXT("remapped immutable selected field"), CaptureSnapshot(BoundRequests, BoundExport, nullptr, nullptr,
          ECaptureValueMode::AuthoringExportLiterals))) return false;
  TestTrue(TEXT("same remapped value and owning snapshot identity"),
      BoundDefault.Snapshot.ReadAuthoringEntry(0).TargetIdentity.CanonicalKey == BoundExport.Snapshot.ReadAuthoringEntry(0).TargetIdentity.CanonicalKey
      && AreStableCanonicalValuesEqual(BoundDefault.Snapshot.ReadAuthoringEntry(0).Value,
          UeAgentPropertyValueCodec::ProjectDirectAuthoringValue(*BoundExport.Snapshot.ReadAuthoringEntry(0).DirectAuthoringValue)));
  FReadRequest Wrong = Bound;
  if (!UeAgentAuthoringTargetLocator::BuildPropertyIdentity(ActorRoot, TEXT("DifferentField"), ActorRoot,
      Wrong.TargetIdentity, Error)) return false;
  TArray<FReadRequest> WrongRequests{Wrong}; FCaptureResult WrongDefault, WrongExport;
  TestFalse(TEXT("default rejects actual native binding mismatch"), CaptureSnapshot(WrongRequests, WrongDefault));
  TestFalse(TEXT("immutable path rejects the same mismatch"), CaptureSnapshot(WrongRequests, WrongExport, nullptr, nullptr,
      ECaptureValueMode::AuthoringExportLiterals));
  TestTrue(TEXT("same mismatch diagnostic and current key evidence"),
      WrongDefault.Diagnostic.Code == WrongExport.Diagnostic.Code && WrongDefault.Diagnostic.PublicCause && WrongExport.Diagnostic.PublicCause
      && WrongDefault.Diagnostic.PublicCause->Code == WrongExport.Diagnostic.PublicCause->Code
      && AreStableCanonicalValuesEqual(WrongDefault.Diagnostic.PublicCause->ActualEvidence, WrongExport.Diagnostic.PublicCause->ActualEvidence)
      && AreStableCanonicalValuesEqual(WrongDefault.Diagnostic.PublicCause->ExpectedEvidence, WrongExport.Diagnostic.PublicCause->ExpectedEvidence));
  UeAgentAuthoringIdentity::FCanonicalIdentity Escaped;
  const FString OwnerPath(TEXT(" /root/中文%| ")), PropertyPath(TEXT("A|B&%"));
  TMap<FString, FString> OriginalSelectors;
  OriginalSelectors.Add(TEXT("owner_path"), OwnerPath);
  OriginalSelectors.Add(TEXT("property_path"), PropertyPath);
  OriginalSelectors.Add(TEXT("notification_owner_path"), OwnerPath);
  if (!UeAgentAuthoringIdentity::BuildCanonicalIdentity(TEXT("property_identity"), TEXT("property"),
      OriginalSelectors, Escaped, Error)) return false;
  bool bSame = false; FString ActualKey;
  TestTrue(TEXT("same original grammar consumes borrowed selector input"),
      UeAgentAuthoringTargetLocator::ComparePropertyIdentity(OwnerPath, PropertyPath, OwnerPath,
          Escaped.CanonicalKey, bSame, ActualKey, Error) && bSame && ActualKey.IsEmpty());
  const FString SavedKey = Escaped.CanonicalKey;
  const FString& AliasedOwner = Escaped.Selectors.FindChecked(TEXT("owner_path"));
  TestTrue(TEXT("owning builder consumes views before replacing aliased output"),
      UeAgentAuthoringTargetLocator::BuildPropertyIdentity(AliasedOwner, PropertyPath, AliasedOwner, Escaped, Error)
      && Escaped.CanonicalKey == SavedKey);
  return true;
}
#endif

FString MakeEntryKey(const FString &SemanticId, const FString &CanonicalKey) {
  return FString::Printf(TEXT("%d:%s%s"), SemanticId.Len(), *SemanticId,
                         *CanonicalKey);
}

// The existing duplicate set borrows the immutable sealed entry, not a second
// concatenation of its long identity. The semantic length and each %s's actual
// first-NUL spelling preserve equality of the original Printf material, including
// case-insensitive comparison. Complete text is produced only for diagnostics.
struct FEntryKeyView {
  FStringView Semantic, Target;
  int32 SemanticLength;
  FEntryKeyView(const FStateEntry& Entry)
      : Semantic(*Entry.SemanticId, FCString::Strlen(*Entry.SemanticId)),
        Target(*Entry.TargetIdentity.CanonicalKey, FCString::Strlen(*Entry.TargetIdentity.CanonicalKey)),
        SemanticLength(Entry.SemanticId.Len()) {}
  friend bool operator==(const FEntryKeyView& A, const FEntryKeyView& B) {
    if (A.SemanticLength != B.SemanticLength || A.Semantic.Len() + A.Target.Len() != B.Semantic.Len() + B.Target.Len()) return false;
    FStringView Left[] = {A.Semantic, A.Target}, Right[] = {B.Semantic, B.Target};
    int32 L = 0, R = 0;
    while (L < 2 && R < 2) {
      if (Left[L].IsEmpty()) { ++L; continue; }
      if (Right[R].IsEmpty()) { ++R; continue; }
      const int32 Count = FMath::Min(Left[L].Len(), Right[R].Len());
      if (!Left[L].Left(Count).Equals(Right[R].Left(Count), ESearchCase::IgnoreCase)) return false;
      Left[L] = Left[L].RightChop(Count); Right[R] = Right[R].RightChop(Count);
    }
    return true;
  }
  friend uint32 GetTypeHash(const FEntryKeyView& Key) {
    uint32 Hash = ::GetTypeHash(Key.SemanticLength);
    for (const FStringView Part : {Key.Semantic, Key.Target})
      for (const TCHAR Ch : Part) Hash = (Hash ^ static_cast<uint32>(TChar<TCHAR>::ToUpper(Ch))) * 16777619u;
    return Hash; // Only a set bucket selector; equality above proves the full key.
  }
};

FStateEntry CloneStateEntry(const FStateEntry &Entry) {
  FStateEntry Clone = Entry;
  Clone.MaterializeDeclaration();
  Clone.Observation.Reset();
  Clone.Value = CloneJsonValue(Entry.Value);
  Clone.ValueDefinition = CloneJsonValue(Entry.ValueDefinition);
  Clone.ComparisonValueDefinition = CloneJsonValue(Entry.ComparisonValueDefinition);
	Clone.CandidatesHeader = CloneJsonValue(Entry.CandidatesHeader);
	Clone.Candidates = CloneJsonValue(Entry.Candidates);
  return Clone;
}

static bool SamePropertyOutputMetadata(const FStateEntry& Previous,
    const UeAgentAuthoringReflection::FDescriptor& Current, bool bEditable,
    const FString& Reason, const FString& ValueText) {
  if (!Previous.Observation || !Previous.bAvailable || !Previous.bAuthoringVisible
      || Previous.bEditable != bEditable || Previous.Candidates || Previous.CandidatesHeader
      || !Previous.Provenance.Equals(TEXT("shared_property_accessor"), ESearchCase::CaseSensitive)
      || !Previous.AccessorId.Equals(TEXT("core.property"), ESearchCase::CaseSensitive)
      || !Previous.DisplayName.Equals(Current.DisplayName, ESearchCase::CaseSensitive)
      || !Previous.Tooltip.Equals(Current.Tooltip, ESearchCase::CaseSensitive)
      || !Previous.AvailabilityReason.Equals(Reason, ESearchCase::CaseSensitive)
      || !Previous.ValueText.Equals(ValueText, ESearchCase::CaseSensitive)
      || !SameStateDescriptorAuthoringContent(Previous.GetType(), Current.GetType())
      || Previous.GetConstraints().Num() != Current.GetConstraints().Num()) return false;
  for (const auto& Item : Current.GetConstraints()) {
    const auto* Before = Previous.GetConstraints().Find(Item.Key);
    if (!Before || !Before->Equals(Item.Value, ESearchCase::CaseSensitive)) return false;
  }
  const auto SameArray = [](const TArray<FString>& A, const TArray<FString>& B) {
    if (A.Num() != B.Num()) return false;
    for (int32 I = 0; I < A.Num(); ++I) if (!A[I].Equals(B[I], ESearchCase::CaseSensitive)) return false;
    return true;
  };
  return SameArray(Previous.GetCategoryPath(), Current.GetCategoryPath()) && SameArray(Previous.PropertyFlags, Current.PropertyFlags);
}

EReadOutcome ReadPropertyStateImpl(const FReadRequest &Request,
    FStateValue &OutValue, FDiagnostic& OutError,
    const FStateEntry* Previous = nullptr, const FStateNativeBinding* PreviousBinding = nullptr) {
  const double ReadStarted = ActiveCaptureTiming ? FPlatformTime::Seconds() : 0;
  UeAgentAuthoringTargetLocator::FResolvedPropertyTarget Target;
  UeAgentAuthoringTargetLocator::FResolveResult TargetResult;
  bool bResolved = false;
  const UeAgentAuthoringIdentity::FCanonicalIdentity* ScopedIdentity = nullptr;
  if (Request.SelectedRootProperty) {
    // The original class/instance selection already supplied this exact field.
    // Do not split its name and perform another reflection search. Native
    // identity is still rebuilt from the actual current owner and compared.
    auto* Object = const_cast<UObject*>(Request.RootObject);
    if (!IsValid(Object) || Object->GetClass() != Request.SelectedRootClass
        || !Request.PropertyPath.Equals(Request.SelectedRootProperty->GetName(), ESearchCase::CaseSensitive)) {
      OutError = StateFailure(TEXT("state_selected_property_binding_changed"),
          TEXT("The current object/class or selected top-level field changed before capture. Producer=property read selection; no alternate property resolver was selected."),
          Request.PropertyPath, DescriptorValue(FString(TEXT("same selected object class and field"))), DescriptorValue(FString(TEXT("binding changed"))));
      return EReadOutcome::Failed;
    }
    Target.NotificationOwner = Object;
    Target.Property = Target.RootProperty = Request.SelectedRootProperty;
    Target.PropertyScope = Request.SelectedRootClass;
    Target.ValuePtr = Target.Property->ContainerPtrToValuePtr<void>(Object);
    Target.NotificationChain.Add(Target.Property);
    const FString CurrentOwnerPath = Object->GetPathName();
    if (bAuthoringExportLiterals && Request.SnapshotIdentityOverride.IsSet()) {
      bool bMatches = false;
      bResolved = UeAgentAuthoringTargetLocator::ComparePropertyIdentity(
          CurrentOwnerPath, Request.PropertyPath, CurrentOwnerPath,
          Request.TargetIdentity.CanonicalKey, bMatches, Target.Identity.CanonicalKey, OutError);
      if (bResolved && bMatches) ScopedIdentity = &Request.TargetIdentity;
    } else bResolved = UeAgentAuthoringTargetLocator::BuildPropertyIdentity(
        CurrentOwnerPath, Request.PropertyPath, CurrentOwnerPath, Target.Identity, OutError);
    if (!bResolved) return EReadOutcome::Failed;
  } else {
    bResolved =
      Request.RootObject
          ? UeAgentAuthoringTargetLocator::ResolvePropertyPath(
                const_cast<UObject *>(Request.RootObject), Request.PropertyPath,
                Target, TargetResult)
          : UeAgentAuthoringTargetLocator::ResolvePropertyPath(
                Request.RootStruct, const_cast<void *>(Request.RootData),
                Request.RootOwnerPath,
                const_cast<UObject *>(Request.NotificationOwner),
                Request.PropertyPath, Target, TargetResult);
  }
  if (!bResolved) {
    OutError = MoveTemp(TargetResult.Diagnostic);
    return EReadOutcome::Failed;
  }
  if (!ScopedIdentity && Target.Identity.CanonicalKey != Request.TargetIdentity.CanonicalKey) {
    auto ExpectedIdentity = MakeShared<FJsonObject>();
    ExpectedIdentity->SetBoolField(TEXT("matches_requested_identity"), true);
    auto ActualIdentity = MakeShared<FJsonObject>();
    ActualIdentity->SetBoolField(TEXT("matches_requested_identity"), false);
    OutError = StateFailure(TEXT("state_accessor_identity_mismatch"),
        TEXT("The resolved property identity differs from the identity requested by the state accessor."),
        Request.PropertyPath, MakeShared<FJsonValueObject>(ExpectedIdentity), MakeShared<FJsonValueObject>(ActualIdentity));
    OutError.ActualEvidence = MakeShared<FJsonValueString>(Target.Identity.CanonicalKey);
    OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Request.TargetIdentity.CanonicalKey);
    return EReadOutcome::Failed;
  }

  UObject *Owner = Target.NotificationOwner
                       ? Target.NotificationOwner
                       : const_cast<UObject *>(Request.NotificationOwner
                                                   ? Request.NotificationOwner
                                                   : Request.RootObject);
  const double ResolvedAt = ActiveCaptureTiming ? FPlatformTime::Seconds() : 0;
  if (ActiveCaptureTiming && Request.SelectedRootProperty) ++ActiveCaptureTiming->SelectedPropertyReads;
  UeAgentAuthoringReflection::FDescriptor Reflection;
  const auto BuildDescriptor = bAuthoringExportLiterals ? UeAgentAuthoringReflection::BuildAuthoringSharedDeclaration
      : bAuthoringExportCapture ? UeAgentAuthoringReflection::BuildAuthoring : UeAgentAuthoringReflection::Build;
  if (!BuildDescriptor(
          Target.Property, Owner, Target.ValuePtr, Reflection, OutError, Request.Limits.bShowDefaultsOnly, Request.Limits.bUseInstanceEditability, Target.PropertyScope)) {
    return EReadOutcome::Failed;
  }
  TSharedPtr<FJsonValue> Encoded;
  const double DescribedAt = ActiveCaptureTiming ? FPlatformTime::Seconds() : 0;
  bool bReuseEmptyValue = false;
  if (Previous && PreviousBinding
      && (!PreviousBinding->NativeValue || PreviousBinding->NativeValue->GetProperty() == Target.Property)
      && SameStateIdentityAuthoringContent(PreviousBinding->NativeTargetIdentity, Target.Identity)) {
    const auto* Array = CastField<FArrayProperty>(Target.Property);
    if (Array && CastField<FObjectPropertyBase>(Array->Inner)
        && UeAgentAuthoringReflection::IsOwnedObjectProperty(Array->Inner)) {
      if (ActiveCaptureTiming) {
        ++ActiveCaptureTiming->OwnedArrayCandidates;
        if (FScriptArrayHelper(Array, Target.ValuePtr).Num() == 0) ++ActiveCaptureTiming->EmptyOwnedArrays;
      }
      bReuseEmptyValue = UeAgentPropertyValueCodec::IsUnchangedEmptyOwnedObjectArray(
          Target.Property, Target.ValuePtr, Previous->Value, Previous->ValueDefinition);
    }
  }
  if (bReuseEmptyValue) {
    // The prior full capture sealed both forms. Verify the root comparison
    // descriptor too, rather than inferring it from a matching value alone.
    auto ExpectedComparison = MakeShared<FJsonObject>();
    ExpectedComparison->Values = Previous->ValueDefinition->AsObject()->Values;
    ExpectedComparison->SetObjectField(TEXT("type"), Reflection.GetType().ToJsonObject());
    bReuseEmptyValue = AreStableCanonicalValuesEqual(Previous->ComparisonValueDefinition,
        MakeShared<FJsonValueObject>(ExpectedComparison));
  }
  if (ActiveCaptureTiming) ActiveCaptureTiming->EmptyProofSeconds += FPlatformTime::Seconds() - DescribedAt;
  bool bReuseScalarValue = false;
  if (!bReuseEmptyValue && Previous && PreviousBinding && !Request.Limits.AuthorizeProperty
      && !Request.Limits.StructFieldFilter
      && SameStateIdentityAuthoringContent(PreviousBinding->NativeTargetIdentity, Target.Identity)) {
    const double ScalarProofStarted = ActiveCaptureTiming ? FPlatformTime::Seconds() : 0;
    bReuseScalarValue = UeAgentPropertyValueCodec::IsUnchangedBuiltinScalar(
        Target.Property, Target.ValuePtr, Previous->Value, Previous->ValueDefinition);
    if (bReuseScalarValue) {
      auto ExpectedComparison = MakeShared<FJsonObject>();
      ExpectedComparison->SetObjectField(TEXT("type"), UeAgentAuthoringTypeDescriptors::Describe(Target.Property).ToJsonObject());
      bReuseScalarValue = AreStableCanonicalValuesEqual(Previous->ComparisonValueDefinition,
          MakeShared<FJsonValueObject>(ExpectedComparison));
    }
    if (ActiveCaptureTiming) ActiveCaptureTiming->ScalarProofSeconds += FPlatformTime::Seconds() - ScalarProofStarted;
  }
  const bool bReuseValue = bReuseEmptyValue || bReuseScalarValue;
  TSet<const UObject *> ObjectStack;
  if (Owner)
    ObjectStack.Add(Owner);
  if (bReuseValue) {
    Encoded = Previous->Value;
    OutValue.ValueDefinition = Previous->ValueDefinition;
  } else if (!EncodeAuthoringPropertyValue(Target.Property, Target.ValuePtr, Owner,
                                    Request.Limits, 0, ObjectStack, Encoded,
                                    OutError, OutValue.ValueDefinition,
                                    bAuthoringExportLiterals ? &OutValue.DirectAuthoringValue : nullptr)) {
    return EReadOutcome::Failed;
  }

  const double EncodedAt = ActiveCaptureTiming ? FPlatformTime::Seconds() : 0;
  if (ActiveCaptureTiming && !bReuseValue && Encoded && !OutValue.DirectAuthoringValue)
    ++ActiveCaptureTiming->JsonKinds.FindOrAdd(Reflection.GetType().Kind);
  if (ScopedIdentity) {
    OutValue.ScopedTargetIdentity = ScopedIdentity;
    if (ActiveCaptureTiming) ++ActiveCaptureTiming->BorrowedResolvedIdentities;
  } else {
    OutValue.TargetIdentity = MoveTemp(Target.Identity);
    if (ActiveCaptureTiming) ++ActiveCaptureTiming->MovedResolvedIdentities;
  }
  OutValue.Value = Encoded;
  OutValue.ValueProperty = Target.Property;
  if (!bAuthoringExportCapture) {
  OutValue.NativeValue =
      UeAgentAuthoringPropertyValues::FPropertyValueStorage::CopyFrom(
          Target.Property, Target.ValuePtr, OutError);
  if (!OutValue.NativeValue.IsValid())
    return EReadOutcome::Failed;
  }
  if (ActiveCaptureTiming) {
    ++ActiveCaptureTiming->PropertyReads;
    ActiveCaptureTiming->ResolveSeconds += ResolvedAt - ReadStarted;
    ActiveCaptureTiming->DescriptorSeconds += DescribedAt - ResolvedAt;
    ActiveCaptureTiming->EncodeSeconds += EncodedAt - DescribedAt;
    (OutValue.DirectAuthoringValue ? ActiveCaptureTiming->ImmutableEncodeSeconds : ActiveCaptureTiming->JsonEncodeSeconds)
        += EncodedAt - DescribedAt;
    ActiveCaptureTiming->NativeCopySeconds += FPlatformTime::Seconds() - EncodedAt;
  }
  OutValue.bAvailable = true;
  OutValue.bEditable = Reflection.bEditable;
  OutValue.AvailabilityReason = MoveTemp(Reflection.AvailabilityReason);
  if (Request.Limits.AuthorizeProperty && Request.Limits.AuthorizeProperty(Target.Property)) {
    OutValue.bEditable = true;
    OutValue.AvailabilityReason.Reset();
  }
  if (bReuseValue) Target.Property->ExportTextItem_Direct(OutValue.ValueText, Target.ValuePtr, nullptr, Owner, PPF_None);
  bool bReusePayload = false;
  if (bReuseValue) {
    const double MetadataProofStarted = ActiveCaptureTiming ? FPlatformTime::Seconds() : 0;
    bReusePayload = SamePropertyOutputMetadata(*Previous, Reflection, OutValue.bEditable.Get(false),
        OutValue.AvailabilityReason, OutValue.ValueText);
    if (ActiveCaptureTiming) {
      if (bReuseEmptyValue) ActiveCaptureTiming->EmptyMetadataProofSeconds += FPlatformTime::Seconds() - MetadataProofStarted;
      else ActiveCaptureTiming->ScalarMetadataProofSeconds += FPlatformTime::Seconds() - MetadataProofStarted;
    }
  }
  if (bReusePayload) {
    OutValue.ReusedAuthoringObservation = Previous->Observation;
    if (ActiveCaptureTiming) {
      if (bReuseEmptyValue) ++ActiveCaptureTiming->ReusedEmptyArrays;
      else ++ActiveCaptureTiming->ReusedScalars;
    }
    OutError.Reset();
    return EReadOutcome::Resolved;
  }
  if (bReuseValue) UeAgentAuthoringEncodedTypes::FScope::Record(Encoded,
      UeAgentAuthoringTypeDescriptors::Describe(Target.Property),
      OutValue.ValueDefinition ? OutValue.ValueDefinition->AsObject() : nullptr);
  OutValue.Type = MoveTemp(Reflection.Type);
  OutValue.SharedDeclaration = MoveTemp(Reflection.SharedDeclaration);
  OutValue.FieldDeclaration = MoveTemp(Reflection.FieldDeclaration);
  OutValue.Constraints = MoveTemp(Reflection.Constraints);
	OutValue.DisplayName = MoveTemp(Reflection.DisplayName);
	OutValue.Tooltip = MoveTemp(Reflection.Tooltip);
	OutValue.CategoryPath = MoveTemp(Reflection.CategoryPath);
	OutValue.SharedCategoryPath = MoveTemp(Reflection.SharedCategoryPath);
	OutValue.PropertyFlags = MoveTemp(Reflection.PropertyFlags);
	const auto Candidates = OutValue.FieldDeclaration ? OutValue.FieldDeclaration
		: UeAgentAuthoringTypeDescriptors::BuildEnumCandidates(UeAgentAuthoringTypeDescriptors::GetPropertyEnum(Target.Property), false, Target.Property);
	if (Candidates) {
		OutValue.CandidatesHeader = Candidates->TryGetField(TEXT("candidates_header"));
		OutValue.Candidates = Candidates->TryGetField(TEXT("candidates"));
	}

  OutValue.Provenance = TEXT("shared_property_accessor");
  if (!bReuseValue && !bAuthoringExportCapture) Target.Property->ExportTextItem_Direct(OutValue.ValueText, Target.ValuePtr, nullptr, Owner, PPF_None);
  OutValue.bOwnsNormalizedPropertyData = true;
  OutError.Reset();
  return EReadOutcome::Resolved;
}

EReadOutcome ReadPropertyState(const FReadRequest& Request, FStateValue& OutValue, FDiagnostic& OutError) {
  return ReadPropertyStateImpl(Request, OutValue, OutError);
}

EReadOutcome ReadPropertyStateWithBaseline(const FReadRequest& Request, const FStateEntry& Previous,
    const FStateNativeBinding& PreviousBinding, FStateValue& OutValue, FDiagnostic& OutError) {
  return ReadPropertyStateImpl(Request, OutValue, OutError, &Previous, &PreviousBinding);
}

bool ValidateTarget(
    const FReadRequest &Request,
    const UeAgentAuthoringSemanticRegistry::FSemanticDescriptor &Semantic,
    FCaptureResult &OutResult, int32& ReusedCanonicalQueries, int32& DirectIdentityValidations) {
  const UeAgentAuthoringIdentity::FCanonicalIdentity &SemanticIdentity =
      Request.SnapshotIdentityOverride.IsSet()
          ? Request.SnapshotIdentityOverride.GetValue()
          : Request.TargetIdentity;
  if (!Request.TargetIdentity.IsValid() || !SemanticIdentity.IsValid()) {
    const auto& InvalidIdentity = !Request.TargetIdentity.IsValid() ? Request.TargetIdentity : SemanticIdentity;
    OutResult.Diagnostic = FDiagnostic::WrapFailure(TEXT("state_target_identity_invalid"), InvalidIdentity.GetValidationDiagnostic(), TEXT("state_snapshot"));
    OutResult.Diagnostic.PropertyPath = (!Request.TargetIdentity.IsValid() ? TEXT("target_identity.") : TEXT("snapshot_identity.")) + OutResult.Diagnostic.PropertyPath;
    OutResult.Diagnostic.SourceKind = TEXT("native_result");
    return false;
  }
  bool bSemanticCanonicalAlreadyValidated = false;
  if (!Request.TargetQuery.ModelId.IsEmpty() ||
      !Request.TargetQuery.TargetKind.IsEmpty() ||
      !Request.TargetQuery.CanonicalKey.IsEmpty() ||
      !Request.TargetQuery.Selectors.IsEmpty()) {
    const auto& Query = Request.TargetQuery;
    const bool bIdentityQuery = Query.Selectors.IsEmpty()
        && (Query.ModelId.IsEmpty() || Query.ModelId.Equals(Request.TargetIdentity.ModelId, ESearchCase::CaseSensitive))
        && (Query.TargetKind.IsEmpty() || Query.TargetKind.Equals(Request.TargetIdentity.TargetKind, ESearchCase::CaseSensitive))
        && (Query.CanonicalKey.IsEmpty() || Query.CanonicalKey.Equals(Request.TargetIdentity.CanonicalKey, ESearchCase::CaseSensitive));
    if (bIdentityQuery) {
      ++DirectIdentityValidations;
      if (!UeAgentAuthoringTargetLocator::ValidateIdentity(Request.TargetIdentity, OutResult.Diagnostic)) return false;
      bSemanticCanonicalAlreadyValidated = &SemanticIdentity == &Request.TargetIdentity;
    } else {
      UeAgentAuthoringTargetLocator::FTargetQuery CompletedQuery =
          Request.TargetQuery;
      if (CompletedQuery.ModelId.IsEmpty()) {
        CompletedQuery.ModelId = Request.TargetIdentity.ModelId;
      }
      if (CompletedQuery.TargetKind.IsEmpty()) {
        CompletedQuery.TargetKind = Request.TargetIdentity.TargetKind;
      }
      if (CompletedQuery.CanonicalKey.IsEmpty() &&
          CompletedQuery.Selectors.IsEmpty()) {
        CompletedQuery.CanonicalKey = Request.TargetIdentity.CanonicalKey;
      }
      if (!UeAgentAuthoringTargetLocator::ValidateExact(
              CompletedQuery, Request.TargetIdentity, OutResult.Diagnostic)) {
        return false;
      }
      // Only reuse the exact query just validated. Snapshot remapping, selectors
      // without this canonical key, and all failed queries retain their own path.
      bSemanticCanonicalAlreadyValidated =
          &SemanticIdentity == &Request.TargetIdentity &&
          CompletedQuery.ModelId.Equals(SemanticIdentity.ModelId, ESearchCase::CaseSensitive) &&
          CompletedQuery.TargetKind.Equals(SemanticIdentity.TargetKind, ESearchCase::CaseSensitive) &&
          CompletedQuery.CanonicalKey.Equals(SemanticIdentity.CanonicalKey, ESearchCase::CaseSensitive);
    }
  }
  if (SemanticIdentity.ModelId != Semantic.IdentityModel) {
    OutResult.Diagnostic = StateFailure(TEXT("state_identity_model_mismatch"), TEXT("The target identity model does not match the requested semantic."), TEXT("identity_model"), DescriptorValue(Semantic.IdentityModel), DescriptorValue(SemanticIdentity.ModelId));
    return false;
  }
  if (SemanticIdentity.TargetKind != Semantic.TargetKind) {
    OutResult.Diagnostic = StateFailure(TEXT("state_target_kind_mismatch"), TEXT("The target kind does not match the requested semantic."), TEXT("target_kind"), DescriptorValue(Semantic.TargetKind), DescriptorValue(SemanticIdentity.TargetKind));
    return false;
  }

  if (bSemanticCanonicalAlreadyValidated) {
    ++ReusedCanonicalQueries;
    return true;
  }
  ++DirectIdentityValidations;
  return UeAgentAuthoringTargetLocator::ValidateIdentity(SemanticIdentity, OutResult.Diagnostic);
}
} // namespace

class FSnapshotBuilder {
public:
  struct FObserved {
    TSharedRef<const FStateEntry> Authoring;
    TOptional<FStateNativeBinding> Binding;
  };
  static TSharedRef<const FStateEntry> ShareAuthoring(const FStateSnapshot& Snapshot, int32 Index) {
    return Snapshot.Storage->Entries[Index];
  }
  static int32 SetEntries(FStateSnapshot &Snapshot,
                         TArray<FObserved> &&Entries, bool bObservationConsumer = true) {
    Snapshot.Storage = MakeShared<FStateSnapshot::FStorage>();
    Snapshot.Storage->Entries.Reserve(Entries.Num());
    for (auto& Entry : Entries) {
      const int32 Index = Snapshot.Storage->Entries.Add(MoveTemp(Entry.Authoring));
      if (Entry.Binding.IsSet()) Snapshot.Storage->NativeBindings.Add(Index, MoveTemp(Entry.Binding.GetValue()));
    }
    if (bObservationConsumer) Snapshot.SealObservationBlocks();
    // This builder is called after CaptureSnapshotImpl's semantic/identity sort.
    Snapshot.Storage->bOrderedAuthoringEntries = true;
    return Snapshot.Storage->NativeBindings.Num();
  }
  static void SetAuthoringEntries(FStateSnapshot& Snapshot,
                                 TArray<TSharedRef<const FStateEntry>>&& Entries) {
    // These are already the final immutable owners in the original key order.
    // The explicit value-only export has no per-entry native-binding consumer.
    Snapshot.Storage = MakeShared<FStateSnapshot::FStorage>();
    Snapshot.Storage->Entries = MoveTemp(Entries);
    Snapshot.Storage->bOrderedAuthoringEntries = true;
  }
};

bool FStateAccessorDescriptor::SupportsTargetKind(
    const FString &TargetKind) const {
  const FString CanonicalTargetKind = NormalizeId(TargetKind);
  return TargetKinds.Contains(TEXT("*")) ||
         TargetKinds.Contains(CanonicalTargetKind);
}

bool FStateAccessorDescriptor::operator==(
    const FStateAccessorDescriptor &Other) const {
  return AccessorsEqual(*this, Other, nullptr);
}

FStateAccessorRegistry &FStateAccessorRegistry::Get() {
  static FStateAccessorRegistry Registry;
  static const bool bBuiltInsRegistered = []() {
    UE::UAI::Contract::FDiagnostic Error;
    return RegisterBuiltInStateAccessors(Registry, Error);
  }();
  ensureAlwaysMsgf(bBuiltInsRegistered,
                   TEXT("State accessor built-in registration failed."));
  return Registry;
}

bool FStateAccessorRegistry::RegisterAccessor(
    const FStateAccessorDescriptor &Descriptor, FStateAccessor Accessor,
    ERegistrationResult &OutResult, UE::UAI::Contract::FDiagnostic& OutError) {
  return RegisterAccessor(Descriptor, MoveTemp(Accessor), {}, OutResult, OutError);
}

bool FStateAccessorRegistry::RegisterAccessor(const FStateAccessorDescriptor& Descriptor,
    FStateAccessor Accessor, FStateAccessorWithBaseline WithBaseline,
    ERegistrationResult& OutResult, FDiagnostic& OutError) {
  const FStateAccessorDescriptor Canonical = Normalize(Descriptor);
  if (!ValidateAccessor(Canonical, Accessor, OutError))
    return false;
  FWriteScopeLock Lock(RegistryLock);
  if (const FEntry *Existing = Accessors.Find(Canonical.AccessorId)) {
    OutError = StateFailure(TEXT("state_accessor_conflict"), TEXT("The state accessor differs from its existing registration."), FString());
    OutError.Stage = TEXT("semantic_registry"); OutError.CandidateName = Canonical.AccessorId;
    if (!AccessorsEqual(Existing->Descriptor, Canonical, &OutError)) {
      return false;
    }
    OutResult = ERegistrationResult::AlreadyRegistered;
    OutError.Reset();
    return true;
  }
  FEntry &Entry = Accessors.Add(Canonical.AccessorId);
  Entry.Descriptor = Canonical;
  Entry.Accessor = MoveTemp(Accessor);
  Entry.WithBaseline = MoveTemp(WithBaseline);
  OutResult = ERegistrationResult::Registered;
  OutError.Reset();
  return true;
}

bool FStateAccessorRegistry::FindAccessor(
    const FString &AccessorId, FStateAccessorDescriptor &OutDescriptor) const {
  FReadScopeLock Lock(RegistryLock);
  if (const FEntry *Entry = Accessors.Find(NormalizeId(AccessorId))) {
    OutDescriptor = Entry->Descriptor;
    return true;
  }
  return false;
}

bool FStateAccessorRegistry::CopyAccessor(const FString &AccessorId,
                                          FStateAccessor &OutAccessor) const {
  FStateAccessorWithBaseline Unused;
  return CopyAccessor(AccessorId, OutAccessor, Unused);
}

bool FStateAccessorRegistry::CopyAccessor(const FString& AccessorId, FStateAccessor& OutAccessor,
    FStateAccessorWithBaseline& OutWithBaseline) const {
  FReadScopeLock Lock(RegistryLock);
  if (const FEntry *Entry = Accessors.Find(NormalizeId(AccessorId))) {
    OutAccessor = Entry->Accessor;
    OutWithBaseline = Entry->WithBaseline;
    return true;
  }
  return false;
}

TArray<FStateAccessorDescriptor> FStateAccessorRegistry::ListAccessors() const {
  FReadScopeLock Lock(RegistryLock);
  TArray<FStateAccessorDescriptor> Result;
  for (const TPair<FString, FEntry> &Pair : Accessors)
    Result.Add(Pair.Value.Descriptor);
  Result.Sort([](const FStateAccessorDescriptor &Left,
                 const FStateAccessorDescriptor &Right) {
    return Left.AccessorId < Right.AccessorId;
  });
  return Result;
}

int32 FStateAccessorRegistry::GetAccessorCount() const {
  FReadScopeLock Lock(RegistryLock);
  return Accessors.Num();
}

bool FStateEntry::IsValid() const {
  return GetValidationDiagnostic().IsEmpty();
}

FDiagnostic FStateEntry::GetValidationDiagnostic() const {
  FDiagnostic Error;
  if (SemanticId.IsEmpty()) Error = MissingStateField(TEXT("state_entry_invalid"), TEXT("The state entry has no semantic identifier."), TEXT("semantic_id"));
  else if (auto IdentityError = TargetIdentity.GetValidationDiagnostic(); !IdentityError.IsEmpty()) {
    Error = FDiagnostic::WrapFailure(TEXT("state_entry_invalid"), IdentityError, TEXT("state_snapshot"));
    Error.PropertyPath = TEXT("target_identity.") + Error.PropertyPath;
    Error.SourceKind = TEXT("native_result");
  }
  else if (GetType().Kind.IsEmpty()) Error = MissingStateField(TEXT("state_entry_invalid"), TEXT("The state entry has no declared value type."), TEXT("type.kind"));
  else if (AccessorId.IsEmpty()) Error = MissingStateField(TEXT("state_entry_invalid"), TEXT("The state entry has no accessor identifier."), TEXT("accessor_id"));
  else if (bAvailable && !Value.IsValid() && !DirectAuthoringValue.IsValid()) Error = MissingStateField(TEXT("state_entry_invalid"), TEXT("An available state entry must contain a value."), TEXT("value"));
  else if (Value.IsValid() && DirectAuthoringValue.IsValid()) Error = MissingStateField(TEXT("state_entry_invalid"), TEXT("A state entry cannot contain two independent current value representations."), TEXT("value"));
  if (!Error.IsEmpty()) Error.SemanticId = SemanticId;
  return Error;
}

TSharedRef<FJsonObject> FStateEntry::ToJsonObject() const {
  TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
  Object->SetStringField(TEXT("semantic_id"), SemanticId);
  Object->SetObjectField(TEXT("target_identity"),
                         TargetIdentity.ToJsonObject());
  if (TSharedPtr<FJsonObject> TypeObject = GetType().ToJsonObject();
      TypeObject.IsValid()) {
    Object->SetObjectField(TEXT("type"), TypeObject);
  }
  TSharedRef<FJsonObject> Availability = MakeShared<FJsonObject>();
  Availability->SetBoolField(TEXT("available"), bAvailable);
  Availability->SetBoolField(TEXT("editable"), bEditable);
  if (!AvailabilityReason.IsEmpty())
    Availability->SetStringField(TEXT("reason"), AvailabilityReason);
  Object->SetObjectField(TEXT("availability"), Availability);
  if (!GetConstraints().IsEmpty()) {
    TSharedRef<FJsonObject> ConstraintObject = MakeShared<FJsonObject>();
    for (const TPair<FString, FString> &Pair : GetConstraints())
      ConstraintObject->SetStringField(Pair.Key, Pair.Value);
    Object->SetObjectField(TEXT("constraints"), ConstraintObject);
  }
	if (!DisplayName.IsEmpty())
		Object->SetStringField(TEXT("display_name"), DisplayName);
	if (!Tooltip.IsEmpty())
		Object->SetStringField(TEXT("tooltip"), Tooltip);
	if (!GetCategoryPath().IsEmpty()) {
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString &Part : GetCategoryPath())
			Values.Add(MakeShared<FJsonValueString>(Part));
		Object->SetArrayField(TEXT("category_path"), Values);
	}
	if (!PropertyFlags.IsEmpty()) {
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString &Flag : PropertyFlags)
			Values.Add(MakeShared<FJsonValueString>(Flag));
		Object->SetArrayField(TEXT("property_flags"), Values);
	}
	if (CandidatesHeader.IsValid())
		Object->SetField(TEXT("candidates_header"), CloneJsonValue(CandidatesHeader));
	if (Candidates.IsValid())
		Object->SetField(TEXT("candidates"), CloneJsonValue(Candidates));
	if (ValueDefinition.IsValid())
		Object->SetField(TEXT("value_definition"), CloneJsonValue(ValueDefinition));
  if (bAvailable && DirectAuthoringValue.IsValid()) {
    Object->SetField(TEXT("value"), UeAgentPropertyValueCodec::ProjectDirectAuthoringValue(*DirectAuthoringValue));
  } else if (bAvailable && Value.IsValid()) Object->SetField(TEXT("value"), CloneJsonValue(Value));
  return Object;
}

TSharedRef<FJsonObject> FStateEntry::ToDiagnosticJsonObject() const {
  TSharedRef<FJsonObject> Object = ToJsonObject();
  Object->SetStringField(TEXT("accessor_id"), AccessorId);
  if (!ValueText.IsEmpty())
    Object->SetStringField(TEXT("value_text"), ValueText);
  if (!Provenance.IsEmpty())
    Object->SetStringField(TEXT("provenance"), Provenance);
  return Object;
}

const FStateNativeBinding& FStateSnapshot::ReadNativeBinding(int32 Index) const {
  if (const auto* Binding = Storage->NativeBindings.Find(Index)) return *Binding;
  static const FStateNativeBinding Empty;
  return Empty;
}

const FStateEntry& FStateSnapshot::ReadEntry(int32 Index) const {
  const auto* Binding = Storage->NativeBindings.Find(Index);
  if (!Binding && !Storage->Entries[Index]->SharedDeclaration && !Storage->Entries[Index]->SharedCategoryPath) return *Storage->Entries[Index];
  FWriteScopeLock Lock(Storage->MaterializationLock);
  if (const auto* Bound = Storage->BoundEntries.Find(Index)) return **Bound;
  auto Bound = MakeShared<FStateEntry>(*Storage->Entries[Index]);
  Bound->MaterializeDeclaration();
  if (Binding) static_cast<FStateNativeBinding&>(*Bound) = *Binding;
  return *Storage->BoundEntries.Add(Index, MoveTemp(Bound));
}

bool FStateSnapshot::CopyEntry(int32 Index, FStateEntry &OutEntry) const {
  const auto Entries = ReadEntries();
  if (!Entries.IsValidIndex(Index))
    return false;
  OutEntry = CloneStateEntry(Entries[Index]);
  return true;
}

bool FStateSnapshot::FindEntry(const FString &SemanticId,
                               const FString &TargetCanonicalKey,
                               FStateEntry &OutEntry) const {
  const auto Entries = ReadEntries();
  const FString CanonicalSemanticId = NormalizeId(SemanticId);
  const int32 Index = FindEntryIndex(CanonicalSemanticId, TargetCanonicalKey);
  if (Index == INDEX_NONE)
    return false;
  OutEntry = CloneStateEntry(Entries[Index]);
  return true;
}

TArray<FStateEntry> FStateSnapshot::CopyEntries() const {
  const auto Entries = ReadEntries();
  TArray<FStateEntry> Result;
  Result.Reserve(Entries.Num());
  for (const FStateEntry &Entry : Entries)
    Result.Add(CloneStateEntry(Entry));
  return Result;
}

const TArray<FStateEntry>& FStateSnapshot::GetEntries() const {
  FWriteScopeLock Lock(Storage->MaterializationLock);
  if (!Storage->MaterializedEntries) {
    auto Entries = MakeShared<TArray<FStateEntry>>();
    Entries->Reserve(Num());
    for (int32 Index = 0; Index < Num(); ++Index) {
      Entries->Add(*Storage->Entries[Index]);
      Entries->Last().MaterializeDeclaration();
      static_cast<FStateNativeBinding&>(Entries->Last()) = ReadNativeBinding(Index);
    }
    Storage->MaterializedEntries = MoveTemp(Entries);
    UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=native_snapshot_legacy_view entries=%d"), Num());
  }
  return *Storage->MaterializedEntries;
}

void FStateSnapshot::ReleaseNativeValues() {
  ReleaseNativeValuesIf([](const FStateEntry&, const FStateNativeBinding&) { return true; });
}

FStateSnapshot FStateSnapshot::MakeAuthoringOnlySnapshot() const {
  FStateSnapshot Result;
  Result.Storage->Entries = Storage->Entries;
  Result.Storage->ObservationMembership = Storage->ObservationMembership;
  Result.Storage->ObservationBlocks = Storage->ObservationBlocks;
  return Result;
}

FStateSnapshot FStateSnapshot::MakeAuthoringObservation(TFunctionRef<const UeAgentAuthoringIdentity::FCanonicalIdentity*(
    const FStateEntry&, const FStateNativeBinding&)> SelectIdentity) const {
  FStateSnapshot Result = MakeAuthoringOnlySnapshot();
  bool bRebound = false;
  for (int32 Index = 0; Index < Num(); ++Index) {
    const auto& Entry = ReadAuthoringEntry(Index);
    const auto* Identity = SelectIdentity(Entry, ReadNativeBinding(Index));
    if (!Identity || Identity->CanonicalKey == Entry.TargetIdentity.CanonicalKey) continue;
    auto Rebound = MakeShared<FStateEntry>(Entry);
    Rebound->TargetIdentity = *Identity;
    Rebound->Observation = MakeShared<const uint8>(0);
    Result.Storage->Entries[Index] = MoveTemp(Rebound);
    bRebound = true;
  }
  if (bRebound) {
    Result.Storage->Entries.Sort([](const TSharedRef<const FStateEntry>& A, const TSharedRef<const FStateEntry>& B) {
      return A->SemanticId != B->SemanticId ? A->SemanticId < B->SemanticId : A->TargetIdentity.CanonicalKey < B->TargetIdentity.CanonicalKey;
    });
    Result.SealObservationBlocks();
  }
  return Result;
}

void FStateSnapshot::ReleaseNativeValuesIf(TFunctionRef<bool(const FStateEntry&)> ShouldRelease) {
  ReleaseNativeValuesIf([&](const FStateEntry& Entry, const FStateNativeBinding& Binding) {
    FStateEntry Bound = Entry;
    static_cast<FStateNativeBinding&>(Bound) = Binding;
    return ShouldRelease(Bound);
  });
}

void FStateSnapshot::ReleaseNativeValuesIf(TFunctionRef<bool(const FStateEntry&, const FStateNativeBinding&)> ShouldRelease) {
  // Evaluate the caller's predicate once, in original order. Detach only for
  // the first real release; a second GC pass over cleared storage is read-only.
  for (int32 Index = 0; Index < Storage->Entries.Num(); ++Index) {
    const FStateEntry& Current = ReadAuthoringEntry(Index);
    const auto& Binding = ReadNativeBinding(Index);
    // Callback-free limits contain only plain policy flags. Retiring an object
    // value must not erase that observation context and turn the next capture
    // into a policy change. Real callbacks may retain native owners and retire
    // together with the value; they are never promoted to detached proof.
    const bool bReleaseLimits = Binding.NativeReadLimits
      && (Binding.NativeReadLimits->StructFieldFilter || Binding.NativeReadLimits->AuthorizeProperty);
    if (ShouldRelease(Current, Binding) && (Binding.NativeValue || bReleaseLimits)) {
      DetachStorage();
      if (auto* Override = Storage->NativeBindings.FindMutable(Index)) {
        Override->NativeValue.Reset();
        if (bReleaseLimits) Override->NativeReadLimits.Reset();
      }
    }
  }
}

TSharedRef<FJsonObject> FStateSnapshot::ToJsonObject() const {
  const auto Entries = ReadAuthoringEntries();
  TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
  Object->SetStringField(TEXT("schema"), TEXT("uai.state_snapshot.v1"));
  TArray<TSharedPtr<FJsonValue>> EntryValues;
  EntryValues.Reserve(Entries.Num());
  for (const FStateEntry &Entry : Entries) {
    EntryValues.Add(MakeShared<FJsonValueObject>(Entry.ToJsonObject()));
  }
  Object->SetArrayField(TEXT("entries"), EntryValues);
  return Object;
}

bool FCaptureResult::IsCaptured() const {
  return Status == TEXT("captured") && Diagnostic.IsEmpty();
}

// Same public fields and optional-field presence as FStateEntry::ToJsonObject,
// without cloning the complete value/definition graph just to compare it.
// Keep the existing canonical value key semantics (including numeric handling).
static bool SameStateDescriptorAuthoringContent(
    const UeAgentAuthoringTypeDescriptors::FDescriptor& A,
    const UeAgentAuthoringTypeDescriptors::FDescriptor& B) {
  // Exactly the fields emitted by FDescriptor::ToJsonObject. operator== is a
  // native type comparison and deliberately does not cover ValueSemantics.
  if (!A.Kind.Equals(B.Kind, ESearchCase::CaseSensitive)
      || !A.StructPath.Equals(B.StructPath, ESearchCase::CaseSensitive)
      || !A.EnumPath.Equals(B.EnumPath, ESearchCase::CaseSensitive)
      || !A.AllowedClassPath.Equals(B.AllowedClassPath, ESearchCase::CaseSensitive)
      || A.FixedLength != B.FixedLength || A.bRequiresHashSource != B.bRequiresHashSource
      || A.ValueSemantics.IsValid() != B.ValueSemantics.IsValid()) return false;
  if (A.ValueSemantics != B.ValueSemantics && A.ValueSemantics
      && !AreStableCanonicalValuesEqual(MakeShared<FJsonValueObject>(A.ValueSemantics),
          MakeShared<FJsonValueObject>(B.ValueSemantics))) return false;
  const auto SameChild = [](const auto& X, const auto& Y) {
    return X.IsValid() == Y.IsValid() && (!X || SameStateDescriptorAuthoringContent(*X, *Y));
  };
  return SameChild(A.ElementType, B.ElementType) && SameChild(A.KeyType, B.KeyType)
      && SameChild(A.MappedType, B.MappedType) && SameChild(A.InnerType, B.InnerType);
}

static bool SameStateIdentityAuthoringContent(
    const UeAgentAuthoringIdentity::FCanonicalIdentity& A,
    const UeAgentAuthoringIdentity::FCanonicalIdentity& B) {
  if (!A.ModelId.Equals(B.ModelId, ESearchCase::CaseSensitive)
      || !A.TargetKind.Equals(B.TargetKind, ESearchCase::CaseSensitive)
      || !A.KeyPattern.Equals(B.KeyPattern, ESearchCase::CaseSensitive)
      || !A.CanonicalKey.Equals(B.CanonicalKey, ESearchCase::CaseSensitive)
      || A.Selectors.Num() != B.Selectors.Num()) return false;
  // Do not use GetSelector: it normalizes keys, unlike ToJsonObject.
  for (const auto& X : A.Selectors) {
    bool bFound = false;
    for (const auto& Y : B.Selectors)
      if (X.Key.Equals(Y.Key, ESearchCase::CaseSensitive)
          && X.Value.Equals(Y.Value, ESearchCase::CaseSensitive)) { bFound = true; break; }
    if (!bFound) return false;
  }
  return true;
}

static bool SameStateEntryAuthoringContent(const FStateEntry& A, const FStateEntry& B, const bool bCompareValue = true) {
  const auto TextEqual = [](const FString& X, const FString& Y) {
    return X.Equals(Y, ESearchCase::CaseSensitive);
  };
  const auto ValueEqual = [](const TSharedPtr<FJsonValue>& X, const TSharedPtr<FJsonValue>& Y) {
    if (X.IsValid() != Y.IsValid()) return false;
    return AreStableCanonicalValuesEqual(X, Y);
  };
  const auto CurrentValueEqual = [&]() {
    using namespace UE::UAI::StateSemantic;
    if (A.DirectAuthoringValue && B.DirectAuthoringValue) {
      if (A.DirectAuthoringValue == B.DirectAuthoringValue) return true;
      UE::UAI::ContextResource::FUeAgentResourceTracker Resources(false);
      return CompareValues(A.DirectAuthoringValue.ToSharedRef(), B.DirectAuthoringValue.ToSharedRef(), TEXT("/"), Resources).IsEquivalent();
    }
    if (A.DirectAuthoringValue) return CanonicalJson::MatchesCanonicalProjection(B.Value, *A.DirectAuthoringValue);
    if (B.DirectAuthoringValue) return CanonicalJson::MatchesCanonicalProjection(A.Value, *B.DirectAuthoringValue);
    return ValueEqual(A.Value, B.Value);
  };
  const auto TextArrayEqual = [&](const TArray<FString>& X, const TArray<FString>& Y) {
    if (X.Num() != Y.Num()) return false;
    for (int32 I = 0; I < X.Num(); ++I) if (!TextEqual(X[I], Y[I])) return false;
    return true;
  };
  if (!TextEqual(A.SemanticId, B.SemanticId) || A.bAvailable != B.bAvailable || A.bEditable != B.bEditable
      || !TextEqual(A.AvailabilityReason, B.AvailabilityReason) || !TextEqual(A.DisplayName, B.DisplayName)
      || !TextEqual(A.Tooltip, B.Tooltip) || !TextArrayEqual(A.GetCategoryPath(), B.GetCategoryPath())
      || !TextArrayEqual(A.PropertyFlags, B.PropertyFlags) || A.GetConstraints().Num() != B.GetConstraints().Num()) return false;
  for (const auto& Constraint : A.GetConstraints()) {
    bool bFound = false;
    for (const auto& Other : B.GetConstraints())
      if (TextEqual(Constraint.Key, Other.Key) && TextEqual(Constraint.Value, Other.Value)) { bFound = true; break; }
    if (!bFound) return false;
  }
  return SameStateIdentityAuthoringContent(A.TargetIdentity, B.TargetIdentity)
      && SameStateDescriptorAuthoringContent(A.GetType(), B.GetType())
      && ValueEqual(A.CandidatesHeader, B.CandidatesHeader) && ValueEqual(A.Candidates, B.Candidates)
      && ValueEqual(A.ValueDefinition, B.ValueDefinition) && (!bCompareValue || !A.bAvailable || CurrentValueEqual());
}

bool HaveSameStateEntryDeclaration(const FStateEntry& A, const FStateEntry& B) {
  return A.bAuthoringVisible == B.bAuthoringVisible
      && A.Provenance.Equals(B.Provenance, ESearchCase::CaseSensitive)
      && SameStateEntryAuthoringContent(A, B, false);
}

bool FStateSnapshot::TryFindAuthoringChanges(const FStateSnapshot& After, TArray<int32>& OutIndices) const {
  OutIndices.Reset();
  if (Num() != After.Num()) return false;
  const bool bSameMembership = Storage->ObservationMembership
      && Storage->ObservationMembership == After.Storage->ObservationMembership
      && Storage->ObservationBlocks.Num() == After.Storage->ObservationBlocks.Num();
  int32 BlockIndex = 0;
  for (int32 Index = 0; Index < Num(); ++Index) {
    if (bSameMembership) {
      while (BlockIndex < Storage->ObservationBlocks.Num()
          && Storage->ObservationBlocks[BlockIndex].FirstIndex < Index) ++BlockIndex;
      if (BlockIndex < Storage->ObservationBlocks.Num()) {
        const auto& BeforeBlock = Storage->ObservationBlocks[BlockIndex];
        const auto& AfterBlock = After.Storage->ObservationBlocks[BlockIndex];
        if (BeforeBlock.FirstIndex == Index && BeforeBlock.NumEntries == AfterBlock.NumEntries
            && BeforeBlock.FirstIndex == AfterBlock.FirstIndex && BeforeBlock.Observation
            && BeforeBlock.Observation == AfterBlock.Observation) {
          Index += BeforeBlock.NumEntries - 1;
          ++BlockIndex;
          continue;
        }
      }
    }
    const auto& BeforeEntry = ReadAuthoringEntry(Index);
    const auto& AfterEntry = After.ReadAuthoringEntry(Index);
    if (!BeforeEntry.SemanticId.Equals(AfterEntry.SemanticId, ESearchCase::CaseSensitive)
        || !SameStateIdentityAuthoringContent(BeforeEntry.TargetIdentity, AfterEntry.TargetIdentity)) {
      OutIndices.Reset(); return false;
    }
    if (BeforeEntry.Observation && BeforeEntry.Observation == AfterEntry.Observation) continue;
    if (BeforeEntry.bAuthoringVisible != AfterEntry.bAuthoringVisible
        || !BeforeEntry.Provenance.Equals(AfterEntry.Provenance, ESearchCase::CaseSensitive)
        || !SameStateEntryAuthoringContent(BeforeEntry, AfterEntry)) OutIndices.Add(Index);
  }
  return true;
}

bool FStateSnapshot::TrySelectEntries(const TArray<int32>& Indices, FStateSnapshot& OutSnapshot) const {
  TArray<int32> Ordered = Indices;
  Ordered.Sort();
  FStateSnapshot Selected;
  int32 Previous = INDEX_NONE;
  for (const int32 Index : Ordered) {
    if (Index < 0 || Index >= Num() || Index == Previous) return false;
    const int32 NewIndex = Selected.Storage->Entries.Add(Storage->Entries[Index]);
    if (const auto* Binding = Storage->NativeBindings.Find(Index))
      Selected.Storage->NativeBindings.Add(NewIndex, *Binding);
    Previous = Index;
  }
  Selected.SealObservationBlocks();
  OutSnapshot = MoveTemp(Selected);
  return true;
}

void FStateSnapshot::DetachStorage() {
  if (!Storage.IsUnique()) Storage = MakeShared<FStorage>(*Storage);
  Storage->MaterializedEntries.Reset();
  Storage->BoundEntries.Reset();
}

void FStateSnapshot::SealObservationBlocks() {
  DetachStorage();
  const auto Entries = ReadAuthoringEntries();
  auto& ObservationMembership = Storage->ObservationMembership;
  auto& ObservationBlocks = Storage->ObservationBlocks;
  ObservationMembership.Reset();
  ObservationBlocks.Reset();
  if (Entries.IsEmpty()) return;
  ObservationMembership = MakeShared<const uint8>(0);
  // The complete result already owns its entries. Seal ranges, not another
  // per-entry position table: a larger capture must not lose every proof.
  for (int32 First = 0; First < Entries.Num();) {
    int32 Last = First + 1;
    while (Last < Entries.Num() && Last - First < 256 && Entries[Last].SemanticId == Entries[First].SemanticId) ++Last;
    ObservationBlocks.Add({First, Last - First, MakeShared<const uint8>(0)});
    First = Last;
  }
}

FStateSnapshot FStateSnapshot::MergeAuthoringObservations(const FStateSnapshot& Additional, const bool bPreferAdditional) const {
  return MergeObservations(Additional, bPreferAdditional, false);
}
FStateSnapshot FStateSnapshot::MergeCapturedObservations(const FStateSnapshot& Additional, const bool bPreferAdditional) const {
  return MergeObservations(Additional, bPreferAdditional, true);
}
FStateSnapshot FStateSnapshot::MergeObservations(const FStateSnapshot& Additional,
    const bool bPreferAdditional, const bool bKeepNativeBindings) const {
  if (IsEmpty()) return bKeepNativeBindings ? Additional : Additional.MakeAuthoringOnlySnapshot();
  if (Additional.IsEmpty()) return bKeepNativeBindings ? *this : MakeAuthoringOnlySnapshot();
  FStateSnapshot Merged;
  auto& Entries = Merged.Storage->Entries;
  Entries.Reserve(Num() + Additional.Num());
  const auto Append = [&](const FStateSnapshot& From, int32 Index) {
    const int32 Position = Entries.Add(From.Storage->Entries[Index]);
    if (bKeepNativeBindings) if (const auto* Binding = From.Storage->NativeBindings.Find(Index))
      Merged.Storage->NativeBindings.Add(Position, *Binding);
  };
  int32 Left = 0, Right = 0;
  while (Left < Num() || Right < Additional.Num()) {
    if (Left == Num()) { Append(Additional, Right++); continue; }
    if (Right == Additional.Num()) { Append(*this, Left++); continue; }
    const auto& A = ReadAuthoringEntry(Left);
    const auto& B = Additional.ReadAuthoringEntry(Right);
    // Native capture and FindEntryIndex use FString's case-insensitive order.
    // Keep the existing authoring-only merge grammar unchanged.
    const auto SearchCase = bKeepNativeBindings ? ESearchCase::IgnoreCase : ESearchCase::CaseSensitive;
    const int32 Order = A.SemanticId != B.SemanticId ? A.SemanticId.Compare(B.SemanticId, SearchCase)
        : A.TargetIdentity.CanonicalKey.Compare(B.TargetIdentity.CanonicalKey, SearchCase);
    if (Order < 0) Append(*this, Left++);
    else if (Order > 0) Append(Additional, Right++);
    else { if (bPreferAdditional) Append(Additional, Right); else Append(*this, Left); ++Left; ++Right; }
  }
  Merged.SealObservationBlocks();
  return Merged;
}

bool FStateSnapshot::TryOverlay(const FStateSnapshot& ActualEntries,
    FStateSnapshot& OutSnapshot, FDiagnostic& OutError, const bool bReuseIdenticalObservations) const {
  TArray<int32> ChangedIndices;
  return TryOverlay(ActualEntries, OutSnapshot, OutError, bReuseIdenticalObservations, ChangedIndices);
}

bool FStateSnapshot::TryOverlay(const FStateSnapshot& ActualEntries,
    FStateSnapshot& OutSnapshot, FDiagnostic& OutError, const bool bReuseIdenticalObservations,
    TArray<int32>& OutChangedIndices) const {
  // An alias denotes an already owned result. Prevalidate every replacement
  // before modifying it, preserving the same failure/no-output-change contract.
  if (&OutSnapshot == this)
    return OutSnapshot.TryOverlayInPlace(ActualEntries, OutError, bReuseIdenticalObservations, &OutChangedIndices);
  FStateSnapshot Merged = *this;
  if (!Merged.TryOverlayInPlace(ActualEntries, OutError, bReuseIdenticalObservations, &OutChangedIndices)) return false;
  OutSnapshot = MoveTemp(Merged);
  return true;
}

int32 FStateSnapshot::FindEntryIndex(const FString& SemanticId, const FString& TargetCanonicalKey) const {
  const auto Entries = ReadAuthoringEntries();
  // Capture seals entries in this exact order; overlay only replaces the same
  // identities. A lookup need not allocate another full identity index.
  int32 First = 0, Last = Entries.Num();
  while (First < Last) {
    const int32 Middle = First + (Last - First) / 2;
    const auto& Entry = Entries[Middle];
    const bool bBefore = Entry.SemanticId != SemanticId
        ? Entry.SemanticId < SemanticId : Entry.TargetIdentity.CanonicalKey < TargetCanonicalKey;
    if (bBefore) First = Middle + 1;
    else Last = Middle;
  }
  return Entries.IsValidIndex(First) && Entries[First].SemanticId == SemanticId
      && Entries[First].TargetIdentity.CanonicalKey == TargetCanonicalKey ? First : INDEX_NONE;
}

bool FStateSnapshot::TryOverlayInPlace(const FStateSnapshot& ActualEntries,
    FDiagnostic& OutError, const bool bReuseIdenticalObservations, TArray<int32>* OutChangedIndices) {
  OutError.Reset();
  if (&ActualEntries == this) { if (OutChangedIndices) OutChangedIndices->Reset(); return true; }
  TArray<int32> ReplacementIndices;
  ReplacementIndices.Reserve(ActualEntries.Num());
  for (const FStateEntry& Actual : ActualEntries.ReadAuthoringEntries()) {
    const int32 Index = FindEntryIndex(Actual.SemanticId, Actual.TargetIdentity.CanonicalKey);
    if (Index == INDEX_NONE) {
      OutError = StateFailure(TEXT("state_overlay_identity_missing"),
          TEXT("An independently captured entry is outside the complete baseline."), TEXT("target_identity"));
      OutError.TargetPath = Actual.TargetIdentity.CanonicalKey;
      return false;
    }
    ReplacementIndices.Add(Index);
  }
  TArray<int32> ChangedIndices;
  if (ReplacementIndices.IsEmpty()) { if (OutChangedIndices) OutChangedIndices->Reset(); return true; }
  DetachStorage();
  auto& Entries = Storage->Entries;
  auto& ObservationBlocks = Storage->ObservationBlocks;
  for (int32 ActualIndex = 0; ActualIndex < ActualEntries.Num(); ++ActualIndex) {
    const FStateEntry& Actual = ActualEntries.ReadAuthoringEntry(ActualIndex);
    const auto& ActualBinding = ActualEntries.ReadNativeBinding(ActualIndex);
    const FStateEntry& Previous = *Entries[ReplacementIndices[ActualIndex]];
    if (bReuseIdenticalObservations && Previous.Observation.IsValid()
        && Previous.bAuthoringVisible == Actual.bAuthoringVisible
        && Previous.Provenance == Actual.Provenance
        && (&Previous == &Actual || SameStateEntryAuthoringContent(Previous, Actual))) {
      const int32 Index = ReplacementIndices[ActualIndex];
      Storage->NativeBindings.Add(Index, ActualBinding);
    } else {
      const int32 EntryIndex = ReplacementIndices[ActualIndex];
      int32 FirstBlock = 0, LastBlock = ObservationBlocks.Num();
      while (FirstBlock < LastBlock) {
        const int32 Middle = FirstBlock + (LastBlock - FirstBlock) / 2;
        if (ObservationBlocks[Middle].FirstIndex <= EntryIndex) FirstBlock = Middle + 1;
        else LastBlock = Middle;
      }
      if (FirstBlock > 0) {
        auto& Block = ObservationBlocks[FirstBlock - 1];
        if (EntryIndex < Block.FirstIndex + Block.NumEntries) Block.Observation = MakeShared<const uint8>(0);
      }
      if (Previous.Observation != Actual.Observation) ChangedIndices.Add(ReplacementIndices[ActualIndex]);
      Entries[ReplacementIndices[ActualIndex]] = ActualEntries.Storage->Entries[ActualIndex];
      Storage->NativeBindings.Remove(ReplacementIndices[ActualIndex]);
      if (ActualEntries.Storage->NativeBindings.Contains(ActualIndex)) {
        Storage->NativeBindings.Add(ReplacementIndices[ActualIndex], ActualBinding);
      }
    }
  }
  if (OutChangedIndices) *OutChangedIndices = MoveTemp(ChangedIndices);
  return true;
}

TSharedRef<FJsonObject> FCaptureResult::ToJsonObject() const {
  TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
  Object->SetStringField(TEXT("status"), Status);
  Object->SetStringField(TEXT("error_code"), IsCaptured() ? TEXT("state_snapshot_captured") : Diagnostic.Code);
  if (!SemanticId.IsEmpty()) Object->SetStringField(TEXT("semantic_id"), SemanticId);
  if (!AccessorId.IsEmpty()) Object->SetStringField(TEXT("accessor_id"), AccessorId);
  if (TargetIdentity.IsValid()) Object->SetObjectField(TEXT("target_identity"), TargetIdentity.ToJsonObject());
  if (IsCaptured()) Object->SetObjectField(TEXT("snapshot"), Snapshot.ToJsonObject());
  else if (!Diagnostic.IsEmpty()) Object->SetObjectField(TEXT("diagnostic"), Diagnostic.ToJson());
  return Object;
}

TSharedPtr<FJsonValue>
CloneCanonicalValue(const TSharedPtr<FJsonValue> &Value) {
  return CloneJsonValue(Value);
}

// Sealed direct inputs are scalar envelopes or fixed builtin shapes (at most
// object -> array -> number). Reuse this owner's exact stable token grammar,
// without projecting an immutable value back to JSON or rebuilding sorted keys.
template<typename TSink>
void AppendStableBuiltin(const UE::UAI::StateSemantic::FUeAgentCanonicalValue& Value, TSink& Out) {
  using UE::UAI::StateSemantic::EUeAgentValueKind;
  switch (Value.GetKind()) {
  case EUeAgentValueKind::Object:
    Out += TEXT("o{");
    for (const auto& Field : Value.GetObjectItems()) { AppendStableKey(Field.Key, Out); AppendStableBuiltin(Field.Value.Get(), Out); }
    Out += TEXT("}"); break;
  case EUeAgentValueKind::Array:
    Out += TEXT("a[");
    for (const auto& Item : Value.GetArrayItems()) AppendStableBuiltin(Item.Get(), Out);
    Out += TEXT("]"); break;
  case EUeAgentValueKind::Boolean: AppendStableBoolean(Value.GetBoolean(), Out); break;
  case EUeAgentValueKind::String: AppendStableStringValue(Value.GetText(), Out); break;
  case EUeAgentValueKind::Null: Out += TEXT("null"); break;
  case EUeAgentValueKind::Integer:
  case EUeAgentValueKind::Decimal: {
    double Number = 0; const bool bParsed = LexTryParseString(Number, *Value.GetLexeme());
    AppendStableNumber(Value.GetLexeme(), bParsed, Number, Out); break;
  }
  default: Out += TEXT("invalid"); break;
  }
}

FString MakeStableCanonicalValueKey(const TSharedPtr<FJsonValue> &Value) {
  return StableJsonKey(Value);
}

void VisitStableCanonicalValueMaterial(const TSharedPtr<FJsonValue>& Value, TFunctionRef<void(const FString&)> Append) {
  VisitStableCanonicalValueMaterialView(Value, [&](FStringView Text) { Append(FString(Text)); });
}

void VisitStableCanonicalValueMaterialView(const TSharedPtr<FJsonValue>& Value, TFunctionRef<void(FStringView)> Append) {
  FStableMaterialViewSink Sink{Append};
  AppendStableJson(Value, Sink);
}

void VisitStableCanonicalValueMaterialView(const FStateEntry& Entry, TFunctionRef<void(FStringView)> Append) {
  if (!Entry.DirectAuthoringValue) { VisitStableCanonicalValueMaterialView(Entry.Value, Append); return; }
  FStableMaterialViewSink Sink{Append};
  AppendStableBuiltin(*Entry.DirectAuthoringValue, Sink);
}

bool AreStableCanonicalValuesEqual(const TSharedPtr<FJsonValue>& Left, const TSharedPtr<FJsonValue>& Right) {
  return SameStableJson(Left, Right);
}

bool RegisterBuiltInStateAccessors(FStateAccessorRegistry &Registry,
                                   UE::UAI::Contract::FDiagnostic& OutError) {
  FStateAccessorDescriptor Descriptor;
  Descriptor.AccessorId = TEXT("core.property");
  Descriptor.TargetKinds = {TEXT("property")};
  Descriptor.CapabilityTags = {TEXT("canonical_value"), TEXT("readonly"),
                               TEXT("reflection")};
  ERegistrationResult Result = ERegistrationResult::Registered;
  return Registry.RegisterAccessor(Descriptor, ReadPropertyState, ReadPropertyStateWithBaseline, Result,
                                   OutError);
}

bool BuildPropertyReadRequest(
    const UObject *RootObject, const FString &PropertyPath,
    const FString &SemanticId, FReadRequest &OutRequest,
    UeAgentAuthoringTargetLocator::FResolveResult &OutTargetResult,
    const FString &CanonicalRootOwnerPath) {
  OutRequest = FReadRequest();
  const FString RootOwnerPath = !CanonicalRootOwnerPath.IsEmpty()
      ? CanonicalRootOwnerPath
      : (RootObject ? RootObject->GetPathName() : FString());
  UeAgentAuthoringTargetLocator::FResolvedPropertyTarget Target;
  if (!UeAgentAuthoringTargetLocator::ResolvePropertyPath(
          RootObject ? RootObject->GetClass() : nullptr,
          const_cast<UObject *>(RootObject), RootOwnerPath,
          const_cast<UObject *>(RootObject), PropertyPath, Target,
          OutTargetResult)) {
    return false;
  }
  OutRequest.SemanticId = NormalizeId(SemanticId);
  OutRequest.TargetIdentity = Target.Identity;
  OutRequest.TargetQuery.ModelId = Target.Identity.ModelId;
  OutRequest.TargetQuery.TargetKind = Target.Identity.TargetKind;
  OutRequest.TargetQuery.CanonicalKey = Target.Identity.CanonicalKey;
  OutRequest.RootObject = RootObject;
  OutRequest.RootOwnerPath = RootOwnerPath;
  OutRequest.NotificationOwner = RootObject;
  OutRequest.PropertyPath = Target.Identity.GetSelector(TEXT("property_path"));
  return true;
}

bool BuildStructPropertyReadRequest(
    UStruct *RootStruct, const void *RootData, const FString &RootOwnerPath,
    const UObject *NotificationOwner, const FString &PropertyPath,
    const FString &SemanticId, FReadRequest &OutRequest,
    UeAgentAuthoringTargetLocator::FResolveResult &OutTargetResult) {
  OutRequest = FReadRequest();
  const FString ResolvedOwnerPath =
      !RootOwnerPath.IsEmpty()
          ? RootOwnerPath
          : (NotificationOwner
                 ? NotificationOwner->GetPathName()
                 : (RootStruct ? RootStruct->GetPathName() : FString()));
  UeAgentAuthoringTargetLocator::FResolvedPropertyTarget Target;
  if (!UeAgentAuthoringTargetLocator::ResolvePropertyPath(
          RootStruct, const_cast<void *>(RootData), ResolvedOwnerPath,
          const_cast<UObject *>(NotificationOwner), PropertyPath, Target,
          OutTargetResult)) {
    return false;
  }
  OutRequest.SemanticId = NormalizeId(SemanticId);
  OutRequest.TargetIdentity = Target.Identity;
  OutRequest.TargetQuery.ModelId = Target.Identity.ModelId;
  OutRequest.TargetQuery.TargetKind = Target.Identity.TargetKind;
  OutRequest.TargetQuery.CanonicalKey = Target.Identity.CanonicalKey;
  OutRequest.RootStruct = RootStruct;
  OutRequest.RootData = RootData;
  OutRequest.RootOwnerPath = ResolvedOwnerPath;
  OutRequest.NotificationOwner = NotificationOwner;
  OutRequest.PropertyPath = Target.Identity.GetSelector(TEXT("property_path"));
  return true;
}

static FReadLimits EditableReadLimits(const FReadLimits& Limits) {
  FReadLimits Result = Limits;
  Result.bApplyAuthoringStructFieldPolicy = true;
  return Result;
}

// Exact immutable identity deltas. One selected owner supplies a prototype;
// each property retains only its different spelling. This compresses storage,
// not observations: selection, root and read policy remain capture-local.
struct FReadRequestTextDelta {
  int32 Prefix = 0, Suffix = 0;
  FString Middle;
  void Build(const FString& Base, const FString& Value) {
    while (Prefix < Base.Len() && Prefix < Value.Len() && Base[Prefix] == Value[Prefix]) ++Prefix;
    while (Suffix < Base.Len() - Prefix && Suffix < Value.Len() - Prefix
        && Base[Base.Len() - 1 - Suffix] == Value[Value.Len() - 1 - Suffix]) ++Suffix;
    Middle = Value.Mid(Prefix, Value.Len() - Prefix - Suffix);
  }
  FString Expand(const FString& Base) const {
    FString Result; Result.Reserve(Prefix + Middle.Len() + Suffix);
    Result.AppendChars(*Base, Prefix); Result += Middle;
    Result.AppendChars(*Base + Base.Len() - Suffix, Suffix);
    return Result;
  }
  bool MatchesIdentity(const FString& Base, const FString& Value) const {
    // Keep the existing typed canonical-identity FString comparison semantics.
    return Value.Len() == Prefix + Middle.Len() + Suffix
        && FCString::Strnicmp(*Base, *Value, Prefix) == 0
        && FCString::Strnicmp(*Middle, *Value + Prefix, Middle.Len()) == 0
        && FCString::Strnicmp(*Base + Base.Len() - Suffix, *Value + Prefix + Middle.Len(), Suffix) == 0;
  }
};
static const FString* FindExactRequestField(const TMap<FString, FString>& Fields, const FString& Key) {
  for (const auto& Pair : Fields) if (Pair.Key.Equals(Key, ESearchCase::CaseSensitive)) return &Pair.Value;
  return nullptr;
}
static bool BuildRequestMapDelta(const TMap<FString, FString>& Base, const TMap<FString, FString>& Values,
    TArray<TPair<FString, FString>>& Delta) {
  if (Base.Num() != Values.Num()) return false;
  for (const auto& Pair : Values) {
    const FString* Previous = FindExactRequestField(Base, Pair.Key);
    if (!Previous) return false;
    if (!Previous->Equals(Pair.Value, ESearchCase::CaseSensitive)) Delta.Emplace(Pair.Key, Pair.Value);
  }
  return true;
}
struct FReadRequestIdentityDelta {
  FReadRequestTextDelta Key;
  TArray<TPair<FString, FString>> Selectors;
  bool Build(const UeAgentAuthoringIdentity::FCanonicalIdentity& Base,
      const UeAgentAuthoringIdentity::FCanonicalIdentity& Value) {
    if (!Base.ModelId.Equals(Value.ModelId, ESearchCase::CaseSensitive)
        || !Base.TargetKind.Equals(Value.TargetKind, ESearchCase::CaseSensitive)
        || !Base.KeyPattern.Equals(Value.KeyPattern, ESearchCase::CaseSensitive)
        || !BuildRequestMapDelta(Base.Selectors, Value.Selectors, Selectors)) return false;
    Key.Build(Base.CanonicalKey, Value.CanonicalKey); return true;
  }
  void Apply(UeAgentAuthoringIdentity::FCanonicalIdentity& Value) const {
    Value.CanonicalKey = Key.Expand(Value.CanonicalKey);
    for (const auto& Pair : Selectors) Value.Selectors.Add(Pair.Key, Pair.Value);
  }
  int64 Bytes() const {
    int64 Result = Key.Middle.GetAllocatedSize() + Selectors.GetAllocatedSize();
    for (const auto& Pair : Selectors) Result += Pair.Key.GetAllocatedSize() + Pair.Value.GetAllocatedSize();
    return Result;
  }
};
struct FCompactReadRequestTemplate {
  TSharedPtr<const FReadRequest> Prototype;
  FString PropertyPath;
  FReadRequestIdentityDelta Target, Snapshot;
  FReadRequestTextDelta QueryKey;
  TArray<TPair<FString, FString>> QuerySelectors;
  bool Build(const TSharedPtr<const FReadRequest>& Base, const FReadRequest& Value) {
    if (Value.RootStruct || Value.RootData
        || !Base->SemanticId.Equals(Value.SemanticId, ESearchCase::CaseSensitive)
        || !Base->RootOwnerPath.Equals(Value.RootOwnerPath, ESearchCase::CaseSensitive)
        || !Base->TargetQuery.ModelId.Equals(Value.TargetQuery.ModelId, ESearchCase::CaseSensitive)
        || !Base->TargetQuery.TargetKind.Equals(Value.TargetQuery.TargetKind, ESearchCase::CaseSensitive)
        || Base->SnapshotIdentityOverride.IsSet() != Value.SnapshotIdentityOverride.IsSet()) return false;
    TArray<TPair<FString, FString>> ContextDelta;
    if (!BuildRequestMapDelta(Base->Context, Value.Context, ContextDelta) || !ContextDelta.IsEmpty()
        || !Target.Build(Base->TargetIdentity, Value.TargetIdentity)
        || !BuildRequestMapDelta(Base->TargetQuery.Selectors, Value.TargetQuery.Selectors, QuerySelectors)) return false;
    if (Value.SnapshotIdentityOverride.IsSet()
        && !Snapshot.Build(Base->SnapshotIdentityOverride.GetValue(), Value.SnapshotIdentityOverride.GetValue())) return false;
    Prototype = Base; PropertyPath = Value.PropertyPath;
    QueryKey.Build(Base->TargetQuery.CanonicalKey, Value.TargetQuery.CanonicalKey); return true;
  }
  FReadRequest Expand() const {
    FReadRequest Result = *Prototype;
    Result.PropertyPath = PropertyPath; Target.Apply(Result.TargetIdentity);
    Result.TargetQuery.CanonicalKey = QueryKey.Expand(Result.TargetQuery.CanonicalKey);
    for (const auto& Pair : QuerySelectors) Result.TargetQuery.Selectors.Add(Pair.Key, Pair.Value);
    if (Result.SnapshotIdentityOverride.IsSet()) Snapshot.Apply(Result.SnapshotIdentityOverride.GetValue());
    return Result;
  }
  const FReadRequestTextDelta& SnapshotKey() const { return Prototype->SnapshotIdentityOverride.IsSet() ? Snapshot.Key : Target.Key; }
  const FString& BaseSnapshotKey() const { return Prototype->SnapshotIdentityOverride.IsSet()
      ? Prototype->SnapshotIdentityOverride->CanonicalKey : Prototype->TargetIdentity.CanonicalKey; }
  int64 Bytes() const {
    int64 Result = sizeof(FCompactReadRequestTemplate) + 32 + PropertyPath.GetAllocatedSize()
        + Target.Bytes() + Snapshot.Bytes() + QueryKey.Middle.GetAllocatedSize() + QuerySelectors.GetAllocatedSize();
    for (const auto& Pair : QuerySelectors) Result += Pair.Key.GetAllocatedSize() + Pair.Value.GetAllocatedSize();
    return Result;
  }
};

FReadRequestBinding::FReadRequestBinding(FReadRequest&& Request)
    : RootObject(Request.RootObject) {
  SelectedRootProperty = Request.SelectedRootProperty; SelectedRootClass = Request.SelectedRootClass;
  Request.SelectedRootProperty = nullptr; Request.SelectedRootClass = nullptr;
  Template = MakeShared<const FReadRequest>(MoveTemp(Request));
}
FReadRequestBinding::FReadRequestBinding(TSharedPtr<const FReadRequest> InTemplate,
    const UObject* InRoot, TSharedPtr<const FReadLimits> InLimits)
    : Template(MoveTemp(InTemplate)), RootObject(InRoot), Limits(MoveTemp(InLimits)) {}
FReadRequestBinding::FReadRequestBinding(TSharedPtr<const FCompactReadRequestTemplate> InTemplate,
    const UObject* InRoot, TSharedPtr<const FReadLimits> InLimits)
    : CompactTemplate(MoveTemp(InTemplate)), RootObject(InRoot), Limits(MoveTemp(InLimits)) {}
const FReadRequest& FReadRequestBinding::GetTemplate() const {
  // Compatibility materialization belongs to this short-lived binding, never
  // to the retained template cache or another generation.
  if (!Template) Template = MakeShared<const FReadRequest>(CompactTemplate->Expand());
  return *Template;
}
const FString& FReadRequestBinding::GetSemanticId() const { return CompactTemplate ? CompactTemplate->Prototype->SemanticId : Template->SemanticId; }
const FString& FReadRequestBinding::GetPropertyPath() const { return CompactTemplate ? CompactTemplate->PropertyPath : Template->PropertyPath; }
const FReadLimits& FReadRequestBinding::GetReadLimits() const { return Limits ? *Limits : GetTemplate().Limits; }
bool FReadRequestBinding::MatchesSnapshotIdentityKey(const FString& Key) const {
  if (CompactTemplate) return CompactTemplate->SnapshotKey().MatchesIdentity(CompactTemplate->BaseSnapshotKey(), Key);
  return (Template->SnapshotIdentityOverride.IsSet() ? Template->SnapshotIdentityOverride->CanonicalKey : Template->TargetIdentity.CanonicalKey) == Key;
}
FString FReadRequestBinding::GetSnapshotIdentityKey() const {
  if (CompactTemplate) return CompactTemplate->SnapshotKey().Expand(CompactTemplate->BaseSnapshotKey());
  return Template->SnapshotIdentityOverride.IsSet() ? Template->SnapshotIdentityOverride->CanonicalKey : Template->TargetIdentity.CanonicalKey;
}
FReadRequest FReadRequestBinding::Materialize() const {
  FReadRequest Request = CompactTemplate ? CompactTemplate->Expand() : *Template;
  if (Limits) {
    Request.RootObject = RootObject;
    Request.NotificationOwner = RootObject;
    Request.Limits = *Limits;
  }
  Request.SelectedRootProperty = SelectedRootProperty;
  Request.SelectedRootClass = SelectedRootClass;
  return Request;
}
void FReadRequestBinding::BindSelectedRootProperty(FProperty* Property, const UStruct* RootClass) {
  SelectedRootProperty = Property; SelectedRootClass = RootClass;
}
static void AppendBoundRequest(TArray<FReadRequest>& Requests, FReadRequestBinding&& Binding, FProperty* Property, const UStruct* RootClass) {
  Binding.BindSelectedRootProperty(Property, RootClass);
  Requests.Add(Binding.Materialize());
}
static void AppendBoundRequest(TArray<FReadRequestBinding>& Requests, FReadRequestBinding&& Binding, FProperty* Property, const UStruct* RootClass) {
  Binding.BindSelectedRootProperty(Property, RootClass);
  Requests.Add(MoveTemp(Binding));
}
static void AppendNewBoundRequest(TArray<FReadRequest>& Requests, FReadRequest&& Request,
    const TSharedPtr<const FCompactReadRequestTemplate>&, const UObject*, const TSharedPtr<const FReadLimits>&) {
  Requests.Add(MoveTemp(Request));
}
static void AppendNewBoundRequest(TArray<FReadRequestBinding>& Requests, FReadRequest&& Request,
    const TSharedPtr<const FCompactReadRequestTemplate>& Template, const UObject* Root, const TSharedPtr<const FReadLimits>& Limits) {
  Requests.Emplace(Template, Root, Limits);
  Requests.Last().BindSelectedRootProperty(Request.SelectedRootProperty, Request.SelectedRootClass);
}
struct FReadRequestTemplateScope {
  TSharedPtr<const FReadRequest> Prototype;
  TMap<FString, TSharedPtr<const FCompactReadRequestTemplate>> Templates;
  int64 Bytes = 0;
};
struct FReadRequestTemplateCache {
  TMap<FString, FReadRequestTemplateScope> Templates;
  int32 Count = 0;
  int64 Bytes = 0;
};
static constexpr int64 ReadRequestTemplateBudget = 256ll * 1024 * 1024;
static int64 RequestMapBytes(const TMap<FString, FString>& Map) {
  int64 Bytes = Map.GetAllocatedSize();
  for (const auto& Pair : Map) Bytes += Pair.Key.GetAllocatedSize() + Pair.Value.GetAllocatedSize();
  return Bytes;
}
static int64 RequestIdentityBytes(const UeAgentAuthoringIdentity::FCanonicalIdentity& Identity) {
  return Identity.ModelId.GetAllocatedSize() + Identity.TargetKind.GetAllocatedSize()
      + Identity.KeyPattern.GetAllocatedSize() + Identity.CanonicalKey.GetAllocatedSize()
      + RequestMapBytes(Identity.Selectors);
}
static int64 RequestTemplateBytes(const FReadRequest& Request, const FString& PropertyName) {
  return sizeof(FReadRequest) + sizeof(TPair<FString, TSharedPtr<const FReadRequest>>) + 128
      + PropertyName.GetAllocatedSize() + Request.SemanticId.GetAllocatedSize()
      + RequestIdentityBytes(Request.TargetIdentity) + Request.RootOwnerPath.GetAllocatedSize()
      + Request.PropertyPath.GetAllocatedSize() + RequestMapBytes(Request.Context)
      + Request.TargetQuery.ModelId.GetAllocatedSize() + Request.TargetQuery.TargetKind.GetAllocatedSize()
      + Request.TargetQuery.CanonicalKey.GetAllocatedSize() + RequestMapBytes(Request.TargetQuery.Selectors)
      + (Request.SnapshotIdentityOverride.IsSet() ? RequestIdentityBytes(Request.SnapshotIdentityOverride.GetValue()) : 0);
}
static FReadRequestTemplateCache& ReadRequestTemplates() {
  static thread_local FReadRequestTemplateCache Cache;
  return Cache;
}
void ResetPropertyReadTemplates() { ReadRequestTemplates() = FReadRequestTemplateCache(); }

struct FPropertyReadCandidates {
  struct FField { FProperty* Property; FString Name; };
  TArray<FField> Fields;
  TArray<int32> NameOrder;
  int64 NameCharacters = 0;
};
struct FPropertyReadCandidateState {
  UeAgentAuthoringReflection::FScopedEditDeclarations EditDeclarations;
  FPropertyReadCandidateState* Previous = nullptr;
  TMap<const UClass*, TSharedPtr<const FPropertyReadCandidates>> Classes;
  TMap<const FProperty*, TOptional<bool>> Support;
  int32 FieldCount = 0;
  int64 NameCharacters = 0;
  int64 Objects = 0, CandidateHits = 0, StaticProofs = 0, StaticHits = 0, DynamicChecks = 0;
  double Started = FPlatformTime::Seconds();
  double CandidateSeconds = 0, SelectionSeconds = 0, BindingSeconds = 0;
  int64 SelectedFields = 0, ReusedBindings = 0, NewBindings = 0;
  TSet<FString> UsedTemplateScopes;
};
static thread_local FPropertyReadCandidateState* ActivePropertyCandidates = nullptr;

FScopedPropertyReadCandidates::FScopedPropertyReadCandidates()
    : State(MakeUnique<FPropertyReadCandidateState>()) {
  State->Previous = ActivePropertyCandidates;
  ActivePropertyCandidates = State.Get();
}
FScopedPropertyReadCandidates::~FScopedPropertyReadCandidates() {
  // A complete synchronous consumer defines the current candidate set. Do not
  // clear useful templates in the middle of an over-budget capture; retain the
  // admitted prefix and release scopes this consumer no longer uses afterwards.
  if (State->Previous) State->Previous->UsedTemplateScopes.Append(State->UsedTemplateScopes);
  else {
    auto& Cache = ReadRequestTemplates();
    for (auto It = Cache.Templates.CreateIterator(); It; ++It) {
      if (!State->UsedTemplateScopes.Contains(It.Key())) {
        Cache.Count -= It.Value().Templates.Num(); Cache.Bytes -= It.Value().Bytes;
        It.RemoveCurrent();
      }
    }
  }
  ActivePropertyCandidates = State->Previous;
  UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=property_request_scope objects=%lld class_plans=%d fields=%d candidate_hits=%lld static_support_proofs=%lld static_support_reused=%lld dynamic_support_checks=%lld candidate_s=%.6f selection_s=%.6f binding_s=%.6f selected_fields=%lld reused_bindings=%lld new_bindings=%lld template_count=%d template_bytes=%lld total_s=%.6f"),
      State->Objects, State->Classes.Num(), State->FieldCount, State->CandidateHits,
      State->StaticProofs, State->StaticHits, State->DynamicChecks, State->CandidateSeconds,
      State->SelectionSeconds, State->BindingSeconds, State->SelectedFields, State->ReusedBindings, State->NewBindings,
      ReadRequestTemplates().Count, ReadRequestTemplates().Bytes,
      FPlatformTime::Seconds() - State->Started);
}

static TSharedPtr<const FPropertyReadCandidates> GetPropertyReadCandidates(const UClass* Class) {
  auto* Scope = ActivePropertyCandidates;
  if (Scope) {
    ++Scope->Objects;
    if (const auto* Existing = Scope->Classes.Find(Class)) { ++Scope->CandidateHits; return Existing->ToSharedRef(); }
  }
  const double Started = FPlatformTime::Seconds();
  auto Candidates = MakeShared<FPropertyReadCandidates>();
  for (TFieldIterator<FProperty> It(Class, EFieldIteratorFlags::IncludeSuper); It; ++It) {
    if (!*It) continue;
    const int32 Index = Candidates->Fields.Add({*It, It->GetName()});
    Candidates->NameCharacters += Candidates->Fields[Index].Name.Len();
    Candidates->NameOrder.Add(Index);
  }
  Candidates->NameOrder.Sort([&](int32 A, int32 B) { return Candidates->Fields[A].Name < Candidates->Fields[B].Name; });
  if (Scope) {
    Scope->CandidateSeconds += FPlatformTime::Seconds() - Started;
    // These are this synchronous request's selected class declarations, not
    // optional cross-request cache entries. Size does not select another
    // discovery algorithm or cause the same class to be rediscovered.
    Scope->FieldCount += Candidates->Fields.Num();
    Scope->NameCharacters += Candidates->NameCharacters;
    Scope->Classes.Add(Class, Candidates);
  }
  return Candidates;
}

static bool IsSupportedInPropertyReadScope(const FProperty* Property, const void* ValuePtr, FDiagnostic& Reason) {
  if (auto* Scope = ActivePropertyCandidates) {
    if (const auto* Proof = Scope->Support.Find(Property)) {
      if (Proof->IsSet()) { ++Scope->StaticHits; Reason.Reset(); return Proof->GetValue(); }
    } else {
      bool bSupported = false;
      const bool bIndependent = UeAgentPropertyValueCodec::TryGetValueIndependentAuthoringSupport(Property, bSupported);
      Scope->Support.Add(Property, bIndependent ? TOptional<bool>(bSupported) : TOptional<bool>());
      if (bIndependent) { ++Scope->StaticProofs; Reason.Reset(); return bSupported; }
    }
    ++Scope->DynamicChecks;
  }
  return UeAgentPropertyValueCodec::IsAuthoringSupported(Property, ValuePtr, Reason);
}

template <typename TRequest>
static bool AppendEditableObjectPropertyReadRequestsImpl(
    const UObject *RootObject, const FEditablePropertyReadOptions &Options,
    TArray<TRequest> &OutRequests, FDiagnostic& OutError) {
  OutError.Reset();
  if (!IsValid(RootObject) || Options.SemanticId.TrimStartAndEnd().IsEmpty()) {
    const bool bObjectValid = IsValid(RootObject);
    OutError = FDiagnostic::Failure(TEXT("editable_property_read_input_invalid"),
        bObjectValid ? TEXT("Editable property capture requires a non-empty semantic identity.")
                     : TEXT("Editable property capture requires a valid root object."),
        TEXT("state_snapshot"), Options.SemanticId);
    OutError.SourceKind = TEXT("native_result");
    OutError.PropertyPath = bObjectValid ? TEXT("semantic_identity_present") : TEXT("root_object_valid");
    OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
    OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
    return false;
  }

  const double SelectionStarted = ActivePropertyCandidates ? FPlatformTime::Seconds() : 0;
  const auto Include = [&](FProperty* Property) {
    if (!Property ||
        Options.ExcludedProperties.Contains(Property->GetFName()) ||
        (!UeAgentAuthoringReflection::IsEditableByFlags(
             Property, RootObject, Options.Limits.bShowDefaultsOnly, Options.Limits.bUseInstanceEditability)
         && !(Options.Limits.AuthorizeProperty && Options.Limits.AuthorizeProperty(Property))
         && !UeAgentAuthoringReflection::IsExistingInlineObjectProperty(
             Property, Property->ContainerPtrToValuePtr<void>(RootObject),
             RootObject))) {
      return false;
    }
    const void *ValuePtr = Property->ContainerPtrToValuePtr<void>(RootObject);
	if (Options.IncludeProperty
		&& !Options.IncludeProperty(Property, ValuePtr, RootObject)) {
		return false;
	}
    FDiagnostic SupportReason;
    if (!IsSupportedInPropertyReadScope(Property, ValuePtr, SupportReason))
      return false;
    return true;
  };
  const UClass* SelectionClass = RootObject->GetClass();
  const auto Candidates = GetPropertyReadCandidates(SelectionClass);
  TArray<const FPropertyReadCandidates::FField*> PropertyFields;
    TArray<uint8> Selected;
    Selected.Init(0, Candidates->Fields.Num());
    // Keep the original candidate/predicate order. Only collection into the final
    // name order is shared; no class-wide selection result is retained.
    for (int32 Index = 0; Index < Candidates->Fields.Num(); ++Index)
      if (Include(Candidates->Fields[Index].Property)) Selected[Index] = 1;
    for (int32 Index : Candidates->NameOrder)
      if (Selected[Index]) PropertyFields.Add(&Candidates->Fields[Index]);

  const double BindingsStarted = ActivePropertyCandidates ? FPlatformTime::Seconds() : 0;
  if (ActivePropertyCandidates) {
    ActivePropertyCandidates->SelectionSeconds += BindingsStarted - SelectionStarted;
    ActivePropertyCandidates->SelectedFields += PropertyFields.Num();
  }
  ON_SCOPE_EXIT {
    if (ActivePropertyCandidates) ActivePropertyCandidates->BindingSeconds += FPlatformTime::Seconds() - BindingsStarted;
  };
  // These templates contain identities only, never UObject/FProperty pointers
  // or callbacks. The current property set above is still independently selected
  // from real values, including inline/owned topology and codec support.
  auto& Cache = ReadRequestTemplates();
  auto& Templates = Cache.Templates;
  auto& TemplateCount = Cache.Count;
  FReadRequestTemplateScope* ReusableScope = nullptr;
  TMap<FString, TSharedPtr<const FCompactReadRequestTemplate>>* Reusable = nullptr;
  const FString SelectedSemantic = NormalizeId(Options.SemanticId);
  if (std::is_same_v<TRequest, FReadRequestBinding> && !Options.RequestIdentityScope.IsEmpty() && !PropertyFields.IsEmpty()) {
    const FString RootPath = RootObject->GetPathName();
    const FString Scope = FString::FromInt(RootPath.Len()) + TEXT(":") + RootPath
        + FString::FromInt(SelectedSemantic.Len()) + TEXT(":") + SelectedSemantic + Options.RequestIdentityScope;
    if (ActivePropertyCandidates) ActivePropertyCandidates->UsedTemplateScopes.Add(Scope);
    ReusableScope = Templates.Find(Scope);
    const int64 ScopeBytes = sizeof(FReadRequestTemplateScope) + Scope.GetAllocatedSize() + 128;
    if (!ReusableScope && ScopeBytes <= ReadRequestTemplateBudget - Cache.Bytes) {
      ReusableScope = &Templates.Add(Scope); ReusableScope->Bytes = ScopeBytes; Cache.Bytes += ScopeBytes;
    }
    if (ReusableScope) Reusable = &ReusableScope->Templates;
  }

  const FReadLimits CurrentLimits = EditableReadLimits(Options.Limits);
  const TSharedPtr<const FReadLimits> BoundLimits = MakeShared<const FReadLimits>(CurrentLimits);
  if (!IsValid(RootObject) || RootObject->GetClass() != SelectionClass) {
    OutError = StateFailure(TEXT("state_selected_property_binding_changed"),
        TEXT("The property selection's object class changed before request binding. No alternate field scan was selected."),
        TEXT("root_class"), DescriptorValue(FString(TEXT("same selected root class"))), DescriptorValue(FString(TEXT("binding changed"))));
    return false;
  }
  const FString CurrentRootPath = RootObject->GetPathName();
  for (const auto* Field : PropertyFields) {
    const FString& PropertyName = Field->Name;
    if (Reusable) if (const auto* Template = Reusable->Find(PropertyName)) {
      if (ActivePropertyCandidates) ++ActivePropertyCandidates->ReusedBindings;
      AppendBoundRequest(OutRequests, FReadRequestBinding(*Template, RootObject, BoundLimits), Field->Property, SelectionClass);
      continue;
    }
    FReadRequest Request;
    if (ActivePropertyCandidates) ++ActivePropertyCandidates->NewBindings;
    if (!UeAgentAuthoringTargetLocator::BuildPropertyIdentity(CurrentRootPath, PropertyName, CurrentRootPath, Request.TargetIdentity, OutError)) {
      if (OutError.IsEmpty()) {
        OutError = FDiagnostic::Failure(TEXT("editable_property_read_request_failed"),
            TEXT("The editable object property could not be resolved for snapshot capture."),
            TEXT("state_snapshot"), Options.SemanticId);
        OutError.SourceKind = TEXT("native_result");
        OutError.PropertyPath = PropertyName;
        OutError.ActualValue = MakeShared<FJsonValueString>(PropertyName);
        OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("resolvable editable property"));
      }
      return false;
    }
    // Options may be supplied by an external caller. Reuse only the exact
    // already-normalized spelling; a changed/noncanonical value still goes
    // through the original normalizer rather than trusting a new flag.
    Request.SemanticId = Options.SemanticId.Equals(SelectedSemantic, ESearchCase::CaseSensitive)
        ? SelectedSemantic : NormalizeId(Options.SemanticId);
    Request.RootObject = RootObject; Request.NotificationOwner = RootObject;
    Request.RootOwnerPath = CurrentRootPath; Request.PropertyPath = PropertyName;
    Request.SelectedRootProperty = Field->Property; Request.SelectedRootClass = SelectionClass;
    // This is an already selected native field, not a second user query over
    // the canonical string just built above. Caller-supplied queries retain
    // BuildPropertyReadRequest/ValidateExact and their original diagnostics.
    Request.Limits = CurrentLimits;
    if (Options.BuildSnapshotIdentity) {
      UeAgentAuthoringIdentity::FCanonicalIdentity SnapshotIdentity;
      if (!Options.BuildSnapshotIdentity(Request.PropertyPath, SnapshotIdentity,
                                         OutError)) {
        if (OutError.IsEmpty()) {
          OutError = FDiagnostic::Failure(TEXT("editable_property_snapshot_identity_failed"),
              TEXT("The snapshot identity callback rejected the editable object property without providing a diagnostic."),
              TEXT("state_snapshot"), Options.SemanticId);
          OutError.SourceKind = TEXT("native_result");
          OutError.PropertyPath = Request.PropertyPath;
          OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
          OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
        }
        return false;
      }
      Request.SnapshotIdentityOverride = MoveTemp(SnapshotIdentity);
    }
    if (Reusable) {
      if (!ReusableScope->Prototype) {
        const int64 PrototypeBytes = RequestTemplateBytes(Request, FString());
        if (PrototypeBytes <= ReadRequestTemplateBudget - Cache.Bytes) {
          FReadRequest Prototype = Request;
          Prototype.RootObject = nullptr; Prototype.NotificationOwner = nullptr;
          Prototype.RootStruct = nullptr; Prototype.RootData = nullptr; Prototype.Limits = {};
          Prototype.SelectedRootProperty = nullptr; Prototype.SelectedRootClass = nullptr;
          ReusableScope->Prototype = MakeShared<const FReadRequest>(MoveTemp(Prototype));
          Cache.Bytes += PrototypeBytes; ReusableScope->Bytes += PrototypeBytes;
        }
      }
      auto Compact = MakeShared<FCompactReadRequestTemplate>();
      if (ReusableScope->Prototype && Compact->Build(ReusableScope->Prototype, Request)) {
        const int64 TemplateBytes = Compact->Bytes() + PropertyName.GetAllocatedSize()
            + sizeof(TPair<FString, TSharedPtr<const FCompactReadRequestTemplate>>) + 128;
        if (TemplateBytes <= ReadRequestTemplateBudget - Cache.Bytes) {
          const TSharedPtr<const FCompactReadRequestTemplate> SharedTemplate = Compact;
          Reusable->Add(PropertyName, SharedTemplate); ++TemplateCount;
          Cache.Bytes += TemplateBytes; ReusableScope->Bytes += TemplateBytes;
          AppendNewBoundRequest(OutRequests, MoveTemp(Request), SharedTemplate, RootObject, BoundLimits);
          continue;
        }
      }
    }
    OutRequests.Add(MoveTemp(Request));
  }
  return true;
}

bool AppendEditableObjectPropertyReadRequests(const UObject* RootObject,
    const FEditablePropertyReadOptions& Options, TArray<FReadRequest>& OutRequests, FDiagnostic& OutError) {
  return AppendEditableObjectPropertyReadRequestsImpl(RootObject, Options, OutRequests, OutError);
}
bool AppendEditableObjectPropertyReadRequests(const UObject* RootObject,
    const FEditablePropertyReadOptions& Options, TArray<FReadRequestBinding>& OutRequests, FDiagnostic& OutError) {
  return AppendEditableObjectPropertyReadRequestsImpl(RootObject, Options, OutRequests, OutError);
}

bool AppendEditableStructPropertyReadRequests(
	UStruct *RootStruct, const void *RootData, const FString &RootOwnerPath,
	const UObject *NotificationOwner,
	const FEditablePropertyReadOptions &Options,
	TArray<FReadRequest> &OutRequests, FDiagnostic& OutError) {
	OutError.Reset();
	if (!RootStruct || !RootData || Options.SemanticId.TrimStartAndEnd().IsEmpty()) {
		OutError = FDiagnostic::Failure(TEXT("editable_struct_property_read_input_invalid"),
			!RootStruct ? TEXT("Editable struct property capture requires a root struct type.")
			: !RootData ? TEXT("Editable struct property capture requires root struct data.")
			: TEXT("Editable struct property capture requires a non-empty semantic identity."),
			TEXT("state_snapshot"), Options.SemanticId);
		OutError.SourceKind = TEXT("native_result");
		OutError.PropertyPath = !RootStruct ? TEXT("root_struct_present")
			: !RootData ? TEXT("root_data_present") : TEXT("semantic_identity_present");
		OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
		OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
		return false;
	}

	TArray<FProperty *> Properties;
	for (TFieldIterator<FProperty> It(RootStruct,
			 EFieldIteratorFlags::IncludeSuper); It; ++It) {
		FProperty *Property = *It;
		if (!Property || Options.ExcludedProperties.Contains(Property->GetFName()))
			continue;
		const void *ValuePtr = Property->ContainerPtrToValuePtr<void>(RootData);
		UeAgentAuthoringReflection::FDescriptor Reflection;
		FDiagnostic ReflectionError;
		if (!UeAgentAuthoringReflection::Build(
				Property, const_cast<UObject *>(NotificationOwner), ValuePtr,
				Reflection, ReflectionError, Options.Limits.bShowDefaultsOnly, Options.Limits.bUseInstanceEditability, RootStruct)
			|| !Reflection.bEditable) {
			continue;
		}
		if (Options.IncludeProperty
			&& !Options.IncludeProperty(Property, ValuePtr, NotificationOwner)) {
			continue;
		}
		FDiagnostic SupportReason;
		if (!UeAgentPropertyValueCodec::IsAuthoringSupported(
				Property, ValuePtr, SupportReason)) {
			continue;
		}
		Properties.Add(Property);
	}
	Properties.Sort([](const FProperty &Left, const FProperty &Right) {
		return Left.GetName() < Right.GetName();
	});

	for (FProperty *Property : Properties) {
		FReadRequest Request;
		UeAgentAuthoringTargetLocator::FResolveResult TargetResult;
		if (!BuildStructPropertyReadRequest(
				RootStruct, RootData, RootOwnerPath, NotificationOwner,
				Property->GetName(), Options.SemanticId, Request, TargetResult)) {
			OutError = TargetResult.Diagnostic;
			if (OutError.IsEmpty()) {
				OutError = FDiagnostic::Failure(TEXT("editable_struct_property_read_request_failed"),
					TEXT("The editable struct property could not be resolved for snapshot capture."),
					TEXT("state_snapshot"), Options.SemanticId);
				OutError.SourceKind = TEXT("native_result");
				OutError.PropertyPath = Property->GetName();
				OutError.ActualValue = MakeShared<FJsonValueString>(Property->GetName());
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("resolvable editable property"));
			}
			return false;
		}
		Request.Limits = EditableReadLimits(Options.Limits);
		if (Options.BuildSnapshotIdentity) {
			UeAgentAuthoringIdentity::FCanonicalIdentity SnapshotIdentity;
			if (!Options.BuildSnapshotIdentity(
					Request.PropertyPath, SnapshotIdentity, OutError)) {
				if (OutError.IsEmpty()) {
					OutError = FDiagnostic::Failure(TEXT("editable_struct_property_snapshot_identity_failed"),
						TEXT("The snapshot identity callback rejected the editable struct property without providing a diagnostic."),
						TEXT("state_snapshot"), Options.SemanticId);
					OutError.SourceKind = TEXT("native_result");
					OutError.PropertyPath = Request.PropertyPath;
					OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
					OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				}
				return false;
			}
			Request.SnapshotIdentityOverride = MoveTemp(SnapshotIdentity);
		}
		OutRequests.Add(MoveTemp(Request));
	}
	return true;
}

UObject *ResolveRequestRootObject(
	const FReadRequest &Request, FDiagnostic& OutError) {
	OutError.Reset();
	if (Request.RootObject)
		return const_cast<UObject *>(Request.RootObject);
	FString Path = Request.TargetIdentity.GetSelector(TEXT("object_path"));
	if (Path.IsEmpty())
		Path = Request.TargetIdentity.GetSelector(TEXT("asset_path"));
	if (Path.IsEmpty()) {
		OutError = MissingStateField(TEXT("state_request_object_path_missing"), TEXT("The state request has neither a root object nor an object_path or asset_path selector."), TEXT("object_path"));
		return nullptr;
	}
	if (!Path.Contains(TEXT("."))) {
		const FString Name = FPackageName::GetLongPackageAssetName(Path);
		if (!Name.IsEmpty())
			Path = FString::Printf(TEXT("%s.%s"), *Path, *Name);
	}
	UObject *Object = StaticLoadObject(UObject::StaticClass(), nullptr, *Path);
	if (!IsValid(Object)) {
		OutError = FDiagnostic::Failure(TEXT("state_request_object_not_found"), TEXT("The requested state object could not be found."), TEXT("state_snapshot"));
        OutError.TargetPath = Path;
		return nullptr;
	}
	return Object;
}

static const FReadRequest& CaptureRequest(const FReadRequest& Request, FReadRequest&) { return Request; }
static const FReadRequest& CaptureRequest(const FReadRequestBinding& Binding, FReadRequest& Materialized) {
  Materialized = Binding.Materialize(); return Materialized;
}
using FInternalReadConsumer = TFunctionRef<bool(const FReadRequest&, FReadRequest*)>;
using FInternalReadProducer = TFunctionRef<bool(FInternalReadConsumer, FDiagnostic&)>;
static bool CaptureSnapshotProduced(
    FInternalReadProducer Producer, int32 KnownRequestCount, FCaptureResult &OutResult,
    const UeAgentAuthoringSemanticRegistry::FRegistry *SemanticRegistry,
    const FStateAccessorRegistry *AccessorRegistry, ECaptureValueMode ValueMode,
    const FStateSnapshot* Baseline, double* OutReadSeconds = nullptr, int32* OutRequestCount = nullptr) {
  const double CaptureStarted = FPlatformTime::Seconds();
  int32 RequestCount = KnownRequestCount >= 0 ? KnownRequestCount : 0;
  double ReadSeconds = 0, InputProductionSeconds = 0;
  const bool bExport = ValueMode == ECaptureValueMode::AuthoringExport || ValueMode == ECaptureValueMode::AuthoringExportLiterals;
  const bool bDetailedTiming = KnownRequestCount < 0 || (KnownRequestCount >= 8 && (KnownRequestCount <= 32768 || bExport));
  ON_SCOPE_EXIT { if (OutReadSeconds) *OutReadSeconds = ReadSeconds; if (OutRequestCount) *OutRequestCount = RequestCount; };
  TGuardValue<bool> CapturePurpose(bAuthoringExportCapture, bExport);
  TGuardValue<bool> LiteralPurpose(bAuthoringExportLiterals, ValueMode == ECaptureValueMode::AuthoringExportLiterals);
  FBoundedCaptureTiming NativeTiming;
  auto* PreviousTiming = ActiveCaptureTiming;
  ActiveCaptureTiming = bDetailedTiming ? &NativeTiming : nullptr;
  double RegistrySeconds = 0, AccessorSeconds = 0, ProjectionSeconds = 0, EntrySeconds = 0, SealSeconds = 0, ReuseBindSeconds = 0;
  double TargetValidationSeconds = 0;
  int32 ReusedPayloads = 0, ReusedSemanticRoutes = 0, ReusedAccessorRoutes = 0, ReusedCanonicalQueries = 0, DirectIdentityValidations = 0;
  int32 TransferredSnapshotIdentities = 0, TransferredSemantics = 0, MovedEntryIdentities = 0;
  int32 EntryObservationTokens = 0, StoredNativeBindings = 0, NativeTransferShells = 0;
  ON_SCOPE_EXIT {
    ActiveCaptureTiming = PreviousTiming;
    if (bDetailedTiming) UE_LOG(LogTemp, Display,
        TEXT("UAI export timing phase=state_snapshot_probe requests=%d property_reads=%d registry_s=%.6f accessor_s=%.6f encoded_projection_s=%.6f entry_s=%.6f seal_s=%.6f property_resolve_s=%.6f property_descriptor_s=%.6f property_encode_s=%.6f property_native_copy_s=%.6f total_s=%.6f success=%s fresh_encoded_transfers=%d ordered_normalizations=%d semantic_route_hits=%d accessor_route_hits=%d target_validation_in_registry_s=%.6f reused_canonical_queries=%d selected_property_reads=%d input_production_s=%.6f direct_literal_values=%d direct_enum_values=%d direct_integer_string_values=%d native_identity_moves=%d snapshot_identity_transfers=%d semantic_transfers=%d entry_identity_moves=%d direct_numeric_values=%d duplicate_key_text_copies=0 direct_identity_validations=%d direct_closed_shape_values=%d direct_shape_numbers=%d direct_reference_values=%d direct_sequence_values=%d"),
        RequestCount, NativeTiming.PropertyReads, RegistrySeconds, AccessorSeconds, ProjectionSeconds, EntrySeconds, SealSeconds,
        NativeTiming.ResolveSeconds, NativeTiming.DescriptorSeconds, NativeTiming.EncodeSeconds, NativeTiming.NativeCopySeconds,
        FPlatformTime::Seconds() - CaptureStarted - InputProductionSeconds, OutResult.Status == TEXT("captured") ? TEXT("true") : TEXT("false"),
        NativeTiming.FreshEncodedTransfers, NativeTiming.OrderedNormalizations, ReusedSemanticRoutes, ReusedAccessorRoutes,
        TargetValidationSeconds, ReusedCanonicalQueries, NativeTiming.SelectedPropertyReads, InputProductionSeconds, NativeTiming.DirectLiterals,
        NativeTiming.DirectEnums, NativeTiming.DirectIntegerStrings, NativeTiming.MovedResolvedIdentities,
        TransferredSnapshotIdentities, TransferredSemantics, MovedEntryIdentities, NativeTiming.DirectNumbers, DirectIdentityValidations, NativeTiming.DirectClosedShapes, NativeTiming.DirectShapeNumbers, NativeTiming.DirectReferences, NativeTiming.DirectSequences);
    if (bDetailedTiming) {
      UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=state_snapshot_storage export_values_only=%s entry_observation_tokens=%d native_binding_records=%d observation_blocks=%d native_transfer_shells=%d"),
          bAuthoringExportLiterals ? TEXT("true") : TEXT("false"), EntryObservationTokens, StoredNativeBindings, OutResult.Snapshot.GetObservationBlocks().Num(), NativeTransferShells);
      UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=state_value_destination immutable_encode_s=%.6f json_encode_s=%.6f clock=existing_property_interval family_prequalification_reads=0 direct_declared_shape_rechecks=0 codec_type_records_retired=%lld borrowed_resolved_identities=%d direct_bitmask_values=%d"),
          NativeTiming.ImmutableEncodeSeconds, NativeTiming.JsonEncodeSeconds, NativeTiming.RetiredTypeRecords, NativeTiming.BorrowedResolvedIdentities, NativeTiming.DirectBitmasks);
      TArray<FString> Kinds; NativeTiming.JsonKinds.GetKeys(Kinds); Kinds.Sort();
      for (const auto& Kind : Kinds) UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=state_json_kind kind=%s values=%d"),
          *Kind, NativeTiming.JsonKinds[Kind]);
    }
    if (bDetailedTiming) UE_LOG(LogTemp, Display,
        TEXT("UAI export timing phase=state_capture_reuse owned_array_candidates=%d actual_empty=%d proven_empty=%d reused_payloads=%d empty_proof_in_encode_s=%.6f empty_metadata_proof_in_accessor_s=%.6f reused_binding_s=%.6f proven_scalar=%d scalar_proof_in_encode_s=%.6f scalar_metadata_proof_in_accessor_s=%.6f"),
        NativeTiming.OwnedArrayCandidates, NativeTiming.EmptyOwnedArrays, NativeTiming.ReusedEmptyArrays,
        ReusedPayloads, NativeTiming.EmptyProofSeconds, NativeTiming.EmptyMetadataProofSeconds, ReuseBindSeconds,
        NativeTiming.ReusedScalars, NativeTiming.ScalarProofSeconds, NativeTiming.ScalarMetadataProofSeconds);
  };
  OutResult = FCaptureResult();
  ON_SCOPE_EXIT {
    auto& Diagnostic = OutResult.Diagnostic;
    if (!Diagnostic.IsEmpty()) {
      if (Diagnostic.Stage.IsEmpty()) Diagnostic.Stage = TEXT("state_snapshot");
      if (Diagnostic.SemanticId.IsEmpty()) Diagnostic.SemanticId = OutResult.SemanticId;
      if (Diagnostic.PropertyPath.IsEmpty()) Diagnostic.PropertyPath = OutResult.TargetIdentity.GetSelector(TEXT("property_path"));
      if (Diagnostic.TargetPath.IsEmpty()) Diagnostic.TargetPath = OutResult.TargetIdentity.GetSelector(TEXT("asset_path"));
      if (Diagnostic.TargetPath.IsEmpty()) Diagnostic.TargetPath = OutResult.TargetIdentity.GetSelector(TEXT("owner_path"));
    }
  };
  if (!IsInGameThread()) {
    OutResult.Status = TEXT("failed");
    OutResult.Diagnostic = StateFailure(TEXT("state_snapshot_game_thread_required"), TEXT("Native state capture must run on the game thread."), TEXT("execution_thread"), DescriptorValue(FString(TEXT("game thread"))), DescriptorValue(FString(TEXT("non-game thread"))));
    return false;
  }
  const UeAgentAuthoringSemanticRegistry::FRegistry &Semantics =
      SemanticRegistry ? *SemanticRegistry
                       : UeAgentAuthoringSemanticRegistry::FRegistry::Get();
  UeAgentAuthoringReflection::FScopedDescriptorMetadata MetadataScope;
  const FStateAccessorRegistry &Accessors =
      AccessorRegistry ? *AccessorRegistry : FStateAccessorRegistry::Get();
  TArray<FSnapshotBuilder::FObserved> CapturedEntries;
  TArray<TSharedRef<const FStateEntry>> CapturedAuthoringEntries;
  TSet<FEntryKeyView> EntryKeys;
  // Both registries are append-only: registering an existing key never
  // replaces its descriptor. Keep just the last successful declaration route
  // in this capture; new keys still perform their current authoritative lookup.
  // Callable copying remains with the registry for every request: custom
  // TFunctions may have mutable captures or observable copy construction.
  FString SemanticRouteKey, AccessorRouteKey;
  TOptional<UeAgentAuthoringSemanticRegistry::FSemanticDescriptor> SemanticRoute;
  TOptional<FStateAccessorDescriptor> AccessorRoute;
  bool bReadSucceeded = true;
  const auto ReadOne = [&](const FReadRequest& Request, FReadRequest* Transferred) -> bool {
    if (KnownRequestCount < 0) ++RequestCount;
    if (!bReadSucceeded) return false;
    const double ReadStarted = KnownRequestCount < 0 ? FPlatformTime::Seconds() : 0;
    bool bThisReadSucceeded = false;
    ON_SCOPE_EXIT { if (KnownRequestCount < 0) ReadSeconds += FPlatformTime::Seconds() - ReadStarted; bReadSucceeded = bThisReadSucceeded; };
    const double RequestStarted = bDetailedTiming ? FPlatformTime::Seconds() : 0;
    const bool bExactSemanticRoute = SemanticRoute.IsSet()
        && SemanticRouteKey.Equals(Request.SemanticId, ESearchCase::CaseSensitive);
    const FString NormalizedSemantic = bExactSemanticRoute ? FString() : NormalizeId(Request.SemanticId);
    // A borrowed successful route cannot be reassigned in its matching branch.
    // The failure guard below runs before this request/local storage expires.
    const FString& SemanticId = bExactSemanticRoute ? SemanticRouteKey : NormalizedSemantic;
    // A successful capture clears these diagnostic-only fields at the end.
    // Keep the current request alive until this guard, and copy only on failure.
    ON_SCOPE_EXIT {
      if (OutResult.Status == TEXT("failed")) {
        OutResult.SemanticId = SemanticId;
        if (OutResult.TargetIdentity.CanonicalKey.IsEmpty()) OutResult.TargetIdentity = Request.TargetIdentity;
      }
    };
    if (SemanticRoute.IsSet() && SemanticRouteKey.Equals(SemanticId, ESearchCase::CaseSensitive)) {
      ++ReusedSemanticRoutes;
    } else {
      UeAgentAuthoringSemanticRegistry::FSemanticDescriptor Current;
      if (!Semantics.FindSemantic(SemanticId, Current)) {
        OutResult.Status = TEXT("failed");
        OutResult.Diagnostic = StateFailure(TEXT("state_semantic_unknown"), TEXT("The requested state semantic is not registered."), TEXT("semantic_id"), DescriptorValue(FString(TEXT("registered semantic"))), DescriptorValue(SemanticId));
        return false;
      }
      SemanticRouteKey = SemanticId; SemanticRoute = MoveTemp(Current);
    }
    const auto& Semantic = SemanticRoute.GetValue();
    const double TargetValidationStarted = bDetailedTiming ? FPlatformTime::Seconds() : 0;
    const bool bValidTarget = ValidateTarget(Request, Semantic, OutResult, ReusedCanonicalQueries, DirectIdentityValidations);
    if (bDetailedTiming) TargetValidationSeconds += FPlatformTime::Seconds() - TargetValidationStarted;
    if (!bValidTarget) {
      OutResult.Status = TEXT("failed");
      return false;
    }

    FStateAccessorDescriptor CurrentAccessorDescriptor;
    OutResult.AccessorId = Semantic.StateAccessor;
    const bool bReuseAccessorRoute = AccessorRoute.IsSet()
        && AccessorRouteKey.Equals(Semantic.StateAccessor, ESearchCase::CaseSensitive);
    if (!bReuseAccessorRoute && !Accessors.FindAccessor(Semantic.StateAccessor, CurrentAccessorDescriptor)) {
      OutResult.Status = TEXT("failed");
      OutResult.Diagnostic = StateFailure(TEXT("state_accessor_unknown"), TEXT("The semantic state accessor is not registered or has no callable implementation."), TEXT("accessor_id"), DescriptorValue(FString(TEXT("registered callable accessor"))), DescriptorValue(Semantic.StateAccessor));
      OutResult.Diagnostic.InternalContext = Semantic.StateAccessor;
      return false;
    }
    const auto& AccessorDescriptor = bReuseAccessorRoute ? AccessorRoute.GetValue() : CurrentAccessorDescriptor;
    if (!AccessorDescriptor.SupportsTargetKind(
            Request.TargetIdentity.TargetKind)) {
      OutResult.Status = TEXT("failed");
      OutResult.Diagnostic = StateFailure(TEXT("state_accessor_target_kind_unsupported"), TEXT("The state accessor does not support the requested target kind."), TEXT("target_kind"));
      TArray<TSharedPtr<FJsonValue>> Allowed;
      for (const auto& Kind : AccessorDescriptor.TargetKinds) Allowed.Add(DescriptorValue(Kind));
      OutResult.Diagnostic.ExpectedValue = MakeShared<FJsonValueArray>(Allowed);
      OutResult.Diagnostic.ActualValue = DescriptorValue(Request.TargetIdentity.TargetKind);
      OutResult.Diagnostic.CandidateName = AccessorDescriptor.AccessorId;
      return false;
    }
    FStateAccessor Accessor;
    FStateAccessorWithBaseline AccessorWithBaseline;
    if (!Accessors.CopyAccessor(Semantic.StateAccessor, Accessor, AccessorWithBaseline)) {
      OutResult.Status = TEXT("failed");
      OutResult.Diagnostic = StateFailure(TEXT("state_accessor_unknown"), TEXT("The semantic state accessor is not registered or has no callable implementation."), TEXT("accessor_id"), DescriptorValue(FString(TEXT("registered callable accessor"))), DescriptorValue(Semantic.StateAccessor));
      return false;
    }
    if (bReuseAccessorRoute) ++ReusedAccessorRoutes;
    else {
      AccessorRouteKey = Semantic.StateAccessor;
      AccessorRoute = CurrentAccessorDescriptor;
    }

    int32 PreviousIndex = INDEX_NONE;
    const FStateEntry* Previous = nullptr;
    if (Baseline && AccessorWithBaseline && ValueMode == ECaptureValueMode::Authoring) {
      const auto& Identity = Request.SnapshotIdentityOverride.IsSet() ? Request.SnapshotIdentityOverride.GetValue() : Request.TargetIdentity;
      PreviousIndex = Baseline->FindEntryIndex(SemanticId, Identity.CanonicalKey);
      if (PreviousIndex != INDEX_NONE) {
        const auto& Candidate = Baseline->ReadAuthoringEntry(PreviousIndex);
        if (Candidate.Observation && (!Candidate.bAuthoringVisible || !Request.IsVerificationOnly())
            && SameStateIdentityAuthoringContent(Identity, Candidate.TargetIdentity)
            && Candidate.SemanticId.Equals(SemanticId, ESearchCase::CaseSensitive)
            && Candidate.AccessorId.Equals(AccessorDescriptor.AccessorId, ESearchCase::CaseSensitive)
            && (Semantic.ValueType == TEXT("*") || NormalizeId(Candidate.Type.Kind) == Semantic.ValueType)
            && Identity.ModelId == Semantic.IdentityModel && Identity.TargetKind == Semantic.TargetKind)
          Previous = &Candidate;
      }
    }
    FStateValue StateValue;
    TSharedPtr<FJsonValue> ComparisonValueDefinition;
    FDiagnostic AccessorError;
    EReadOutcome Outcome;
    bool bCanTransferNormalizedValue = false;
    double AccessorFinished = 0;
    {
      UeAgentAuthoringEncodedTypes::FScope EncodedTypes;
      const double AccessorStarted = bDetailedTiming ? FPlatformTime::Seconds() : 0;
      if (bDetailedTiming) RegistrySeconds += AccessorStarted - RequestStarted;
      Outcome = Previous
          ? AccessorWithBaseline(Request, *Previous, Baseline->ReadNativeBinding(PreviousIndex), StateValue, AccessorError)
          : Accessor(Request, StateValue, AccessorError);
      AccessorFinished = bDetailedTiming ? FPlatformTime::Seconds() : 0;
      if (bDetailedTiming) AccessorSeconds += AccessorFinished - AccessorStarted;
      if (Outcome == EReadOutcome::Resolved && !StateValue.ReusedAuthoringObservation && !StateValue.DirectAuthoringValue)
      {
        const auto* BeforeProjection = StateValue.Value.Get();
        if (ValueMode != ECaptureValueMode::Native)
          StateValue.Value = EncodedTypes.ProjectAuthoringValue(StateValue.Value);
        // Projection can omit nested defaults. Preserve the old second
        // normalization when that changes the tree (including map/set order).
        bCanTransferNormalizedValue = StateValue.bOwnsNormalizedPropertyData
            && StateValue.Value.Get() == BeforeProjection;
        if (!bAuthoringExportCapture) if (auto Definition = EncodedTypes.BuildDefinition(StateValue.Value))
        {
          // A document may declare only its collection contract. Keep native
          // leaf provenance too, without duplicating a complete codec definition.
          if (!StateValue.ValueDefinition || StateValue.ValueDefinition->Type != EJson::Object
              || StateValue.ValueDefinition->AsObject() != Definition)
            ComparisonValueDefinition = MakeShared<FJsonValueObject>(Definition);
        }
      }
    }
    const double ProjectionFinished = bDetailedTiming ? FPlatformTime::Seconds() : 0;
    if (bDetailedTiming) ProjectionSeconds += ProjectionFinished - AccessorFinished;
    if (Previous && Outcome == EReadOutcome::Resolved && StateValue.NativeValue
        && StateValue.ReusedAuthoringObservation == Previous->Observation
        && SameStateIdentityAuthoringContent(StateValue.GetTargetIdentity(), Request.TargetIdentity)) {
      bool bDuplicate = false; EntryKeys.Add(FEntryKeyView(*Previous), &bDuplicate);
      if (bDuplicate) {
        OutResult.Status = TEXT("failed");
        OutResult.Diagnostic = StateFailure(TEXT("state_snapshot_duplicate_entry"),
            TEXT("The snapshot contains the same semantic and target more than once."), Previous->TargetIdentity.GetSelector(TEXT("property_path")));
        OutResult.Diagnostic.InternalContext = MakeEntryKey(Previous->SemanticId, Previous->TargetIdentity.CanonicalKey);
        OutResult.AccessorId = Previous->AccessorId;
        OutResult.TargetIdentity = Previous->TargetIdentity;
        return false;
      }
      FStateNativeBinding Current;
      Current.NativeValue = MoveTemp(StateValue.NativeValue);
      Current.NativeTargetIdentity = MoveTemp(StateValue.TargetIdentity);
      Current.NativeReadLimits = MakeShared<FReadLimits>(Request.Limits);
      CapturedEntries.Add({FSnapshotBuilder::ShareAuthoring(*Baseline, PreviousIndex), MoveTemp(Current)});
      ++NativeTransferShells;
      ++ReusedPayloads;
      if (bDetailedTiming) ReuseBindSeconds += FPlatformTime::Seconds() - ProjectionFinished;
      bThisReadSucceeded = true; return true;
    }
    if (Outcome == EReadOutcome::Failed) {
      OutResult.Status = TEXT("failed");
      OutResult.Diagnostic = AccessorError.IsEmpty()
          ? StateFailure(TEXT("state_accessor_failed"), TEXT("The state accessor reported failure without a diagnostic."), TEXT("accessor_id"), nullptr, DescriptorValue(Semantic.StateAccessor))
          : FDiagnostic::WrapFailure(TEXT("state_accessor_failed"), AccessorError, TEXT("state_snapshot"));
      return false;
    }
    if (!StateValue.GetTargetIdentity().IsValid() ||
        StateValue.GetTargetIdentity().CanonicalKey !=
            Request.TargetIdentity.CanonicalKey) {
      OutResult.Status = TEXT("failed");
      const auto IdentityError = StateValue.GetTargetIdentity().GetValidationDiagnostic();
      OutResult.Diagnostic = !IdentityError.IsEmpty()
          ? FDiagnostic::WrapFailure(TEXT("state_accessor_identity_mismatch"), IdentityError, TEXT("state_snapshot"))
          : StateFailure(TEXT("state_accessor_identity_mismatch"), TEXT("The state accessor returned a different target from the requested target."), TEXT("target_identity"));
      OutResult.Diagnostic.ExpectedEvidence = DescriptorValue(Request.TargetIdentity.CanonicalKey);
      OutResult.Diagnostic.ActualEvidence = DescriptorValue(StateValue.GetTargetIdentity().CanonicalKey);
      OutResult.Diagnostic.SourceKind = TEXT("native_result");
      return false;
    }
    if (StateValue.GetType().Kind.IsEmpty())
      StateValue.Type.Kind = Semantic.ValueType;
    if (Semantic.ValueType != TEXT("*") &&
        NormalizeId(StateValue.GetType().Kind) != Semantic.ValueType) {
      OutResult.Status = TEXT("failed");
      OutResult.Diagnostic = StateFailure(TEXT("state_value_type_mismatch"), TEXT("The captured value type does not match the declared semantic type."), TEXT("type.kind"), DescriptorValue(Semantic.ValueType), DescriptorValue(StateValue.GetType().Kind));
      OutResult.Diagnostic.InternalContext = StateValue.GetType().Kind;
      return false;
    }

    if (Request.SnapshotIdentityOverride.IsSet()) {
      const UeAgentAuthoringIdentity::FCanonicalIdentity &SnapshotIdentity =
          Request.SnapshotIdentityOverride.GetValue();
      if (!SnapshotIdentity.IsValid() ||
          SnapshotIdentity.ModelId != Semantic.IdentityModel ||
          SnapshotIdentity.TargetKind != Semantic.TargetKind) {
        OutResult.Status = TEXT("failed");
        const auto IdentityError = SnapshotIdentity.GetValidationDiagnostic();
        if (!IdentityError.IsEmpty()) OutResult.Diagnostic = FDiagnostic::WrapFailure(TEXT("state_snapshot_identity_override_invalid"), IdentityError, TEXT("state_snapshot"));
        else {
          const bool bModelMismatch = SnapshotIdentity.ModelId != Semantic.IdentityModel;
          OutResult.Diagnostic = StateFailure(TEXT("state_snapshot_identity_override_invalid"), TEXT("The snapshot identity override does not match the semantic model or target kind."),
              bModelMismatch ? TEXT("identity_model") : TEXT("target_kind"), DescriptorValue(bModelMismatch ? Semantic.IdentityModel : Semantic.TargetKind), DescriptorValue(bModelMismatch ? SnapshotIdentity.ModelId : SnapshotIdentity.TargetKind));
        }
        OutResult.Diagnostic.SourceKind = TEXT("native_result");
        return false;
      }
    }

    FStateEntry Entry;
    if (Transferred && Request.SemanticId.Equals(SemanticId, ESearchCase::CaseSensitive)) {
      Entry.SemanticId = MoveTemp(Transferred->SemanticId); ++TransferredSemantics;
    } else Entry.SemanticId = SemanticId;
    if (Request.SnapshotIdentityOverride.IsSet()) {
      if (Transferred) { Entry.TargetIdentity = MoveTemp(Transferred->SnapshotIdentityOverride.GetValue()); ++TransferredSnapshotIdentities; }
      else Entry.TargetIdentity = Request.SnapshotIdentityOverride.GetValue();
    } else if (!StateValue.NativeValue || bAuthoringExportCapture) {
      Entry.TargetIdentity = MoveTemp(StateValue.TargetIdentity); ++MovedEntryIdentities;
    } else Entry.TargetIdentity = StateValue.TargetIdentity; // Actual native binding still consumes its separate identity below.
    Entry.Type = MoveTemp(StateValue.Type);
    Entry.SharedDeclaration = MoveTemp(StateValue.SharedDeclaration);
    Entry.FieldDeclaration = MoveTemp(StateValue.FieldDeclaration);
    Entry.AuthoringProjection = MoveTemp(StateValue.AuthoringProjection);
    Entry.DirectAuthoringValue = MoveTemp(StateValue.DirectAuthoringValue);
    Entry.bAvailable =
        Outcome == EReadOutcome::Resolved && StateValue.bAvailable;
    Entry.bEditable = Entry.bAvailable && StateValue.bEditable.Get(
        Semantic.WriteCapability == UeAgentAuthoringSemanticRegistry::EWriteCapability::DomainMutation);
    Entry.bAuthoringVisible = StateValue.bAuthoringVisible && !Request.IsVerificationOnly();
    Entry.AvailabilityReason = MoveTemp(StateValue.AvailabilityReason);
	Entry.Constraints = MoveTemp(StateValue.Constraints);
	Entry.DisplayName = MoveTemp(StateValue.DisplayName);
	Entry.Tooltip = MoveTemp(StateValue.Tooltip);
	Entry.CategoryPath = MoveTemp(StateValue.CategoryPath);
	Entry.SharedCategoryPath = MoveTemp(StateValue.SharedCategoryPath);
	Entry.PropertyFlags = MoveTemp(StateValue.PropertyFlags);
	Entry.CandidatesHeader = Entry.FieldDeclaration ? MoveTemp(StateValue.CandidatesHeader) : CloneJsonValue(StateValue.CandidatesHeader);
	Entry.Candidates = Entry.FieldDeclaration ? MoveTemp(StateValue.Candidates) : CloneJsonValue(StateValue.Candidates);
	Entry.ValueDefinition = StateValue.bOwnsNormalizedPropertyData
      ? MoveTemp(StateValue.ValueDefinition) : CloneJsonValue(StateValue.ValueDefinition);
	Entry.ComparisonValueDefinition = CloneJsonValue(ComparisonValueDefinition);
    Entry.AccessorId = AccessorDescriptor.AccessorId;
    Entry.ValueText = MoveTemp(StateValue.ValueText);
    Entry.Provenance = MoveTemp(StateValue.Provenance);
    if (Outcome == EReadOutcome::Unavailable) {
      Entry.bAvailable = false;
      Entry.bEditable = false;
      if (Entry.AvailabilityReason.IsEmpty())
        Entry.AvailabilityReason = TEXT("state_unavailable");
    } else {
      if (!StateValue.Value.IsValid() && !Entry.DirectAuthoringValue) {
        OutResult.Status = TEXT("failed");
        OutResult.Diagnostic = MissingStateField(TEXT("state_value_missing"), TEXT("The state accessor reported a resolved value but returned no value storage."), TEXT("value"));
        return false;
      }
      if (!Entry.DirectAuthoringValue) Entry.Value =
          bCanTransferNormalizedValue ? MoveTemp(StateValue.Value) : (StateValue.ValueProperty || StateValue.NativeValue.IsValid())
              ? NormalizePropertyValue(StateValue.ValueProperty ? StateValue.ValueProperty : StateValue.NativeValue->GetProperty(),
                                       StateValue.Value)
              : CloneJsonValue(StateValue.Value);
      if (StateValue.NativeValue.IsValid() && !bAuthoringExportCapture) {
        FDiagnostic StorageError;
        TSharedPtr<UeAgentAuthoringPropertyValues::FPropertyValueStorage> Detached;
        if (StateValue.bOwnsNormalizedPropertyData && StateValue.NativeValue.IsUnique()) Detached = MoveTemp(StateValue.NativeValue);
        else Detached = UeAgentAuthoringPropertyValues::FPropertyValueStorage::CopyFrom(
                    StateValue.NativeValue->GetProperty(),
                    StateValue.NativeValue->GetValuePtr(), StorageError);
        if (!Detached.IsValid()) {
          OutResult.Status = TEXT("failed");
          OutResult.Diagnostic = FDiagnostic::WrapFailure(TEXT("state_native_value_copy_failed"), StorageError, TEXT("state_snapshot"));
          return false;
        }
        Entry.NativeValue = Detached;
        Entry.NativeTargetIdentity = MoveTemp(StateValue.TargetIdentity);
        Entry.NativeReadLimits = MakeShared<FReadLimits>(Request.Limits);
      }
    }
    if (!Entry.IsValid()) {
      OutResult.Status = TEXT("failed");
      OutResult.Diagnostic = Entry.GetValidationDiagnostic();
      return false;
    }
    // Establish the actual immutable owner before borrowing either string.
    // This explicit output purpose has no native-baseline/observation consumer.
    // Default and before/after capture still seal their original observations.
    if (!bAuthoringExportLiterals) { Entry.Observation = MakeShared<const uint8>(0); ++EntryObservationTokens; }
    TOptional<FStateNativeBinding> Binding;
    if (!bAuthoringExportLiterals) {
      Binding.Emplace(MoveTemp(static_cast<FStateNativeBinding&>(Entry)));
      static_cast<FStateNativeBinding&>(Entry) = FStateNativeBinding();
    }
    const auto Owned = MakeShared<const FStateEntry>(MoveTemp(Entry));
    bool bDuplicate = false; EntryKeys.Add(FEntryKeyView(*Owned), &bDuplicate);
    if (bDuplicate) {
      OutResult.Status = TEXT("failed");
      OutResult.Diagnostic = StateFailure(TEXT("state_snapshot_duplicate_entry"), TEXT("The snapshot contains the same semantic and target more than once."), Owned->TargetIdentity.GetSelector(TEXT("property_path")));
      OutResult.Diagnostic.InternalContext = MakeEntryKey(Owned->SemanticId, Owned->TargetIdentity.CanonicalKey);
      OutResult.SemanticId = Owned->SemanticId;
      OutResult.AccessorId = Owned->AccessorId;
      OutResult.TargetIdentity = Owned->TargetIdentity;
      return false;
    }
    // Entry already owns freshly cloned/normalized value, definitions and
    // candidate trees above. Seal that ownership instead of cloning them again.
    if (bAuthoringExportLiterals) CapturedAuthoringEntries.Add(Owned);
    else { CapturedEntries.Add({Owned, MoveTemp(Binding)}); ++NativeTransferShells; }
    if (bDetailedTiming) EntrySeconds += FPlatformTime::Seconds() - ProjectionFinished;
    bThisReadSucceeded = true;
    return true;
  };
  FDiagnostic ProducerError;
  const double ProducerStarted = KnownRequestCount < 0 ? FPlatformTime::Seconds() : 0;
  const bool bProduced = Producer(ReadOne, ProducerError);
  if (KnownRequestCount < 0) InputProductionSeconds = FPlatformTime::Seconds() - ProducerStarted - ReadSeconds;
  if (!bProduced) {
    OutResult = FCaptureResult(); OutResult.Status = TEXT("failed");
    OutResult.Diagnostic = ProducerError.IsEmpty()
        ? StateFailure(TEXT("state_request_producer_failed"), TEXT("The request producer failed without a diagnostic."), TEXT("request_producer"))
        : MoveTemp(ProducerError);
    return false;
  }
  if (!bReadSucceeded) return false;

  const double SealStarted = bDetailedTiming ? FPlatformTime::Seconds() : 0;
  const auto EntryLess = [](const FStateEntry& Left, const FStateEntry& Right) {
    if (Left.SemanticId != Right.SemanticId)
      return Left.SemanticId < Right.SemanticId;
    return Left.TargetIdentity.CanonicalKey < Right.TargetIdentity.CanonicalKey;
  };
  if (bAuthoringExportLiterals) {
    CapturedAuthoringEntries.Sort([&](const auto& A, const auto& B) { return EntryLess(*A, *B); });
    FSnapshotBuilder::SetAuthoringEntries(OutResult.Snapshot, MoveTemp(CapturedAuthoringEntries));
  } else {
    CapturedEntries.Sort([&](const auto& A, const auto& B) { return EntryLess(*A.Authoring, *B.Authoring); });
    StoredNativeBindings = FSnapshotBuilder::SetEntries(OutResult.Snapshot, MoveTemp(CapturedEntries));
  }
  if (bDetailedTiming) SealSeconds = FPlatformTime::Seconds() - SealStarted;
  OutResult.Status = TEXT("captured");
  OutResult.Diagnostic.Reset();
  OutResult.SemanticId.Reset();
  OutResult.AccessorId.Reset();
  OutResult.TargetIdentity = UeAgentAuthoringIdentity::FCanonicalIdentity();
  return true;
}

template <typename TRequest>
static bool CaptureSnapshotImpl(const TArray<TRequest>& Requests, FCaptureResult& OutResult,
    const UeAgentAuthoringSemanticRegistry::FRegistry* Semantics,
    const FStateAccessorRegistry* Accessors, ECaptureValueMode ValueMode, const FStateSnapshot* Baseline) {
  const auto Produce = [&](FInternalReadConsumer Consume, FDiagnostic&) {
    for (const auto& Input : Requests) {
      FReadRequest Materialized;
      const auto& Request = CaptureRequest(Input, Materialized);
      if (!Consume(Request, &Request == &Materialized ? &Materialized : nullptr)) break;
    }
    return true;
  };
  return CaptureSnapshotProduced(Produce, Requests.Num(), OutResult, Semantics, Accessors, ValueMode, Baseline);
}

bool CaptureSnapshot(FReadRequestProducer Producer, FCaptureResult& OutResult,
    ECaptureValueMode ValueMode, double* OutReadSeconds, int32* OutRequestCount) {
  const auto Borrow = [&](FInternalReadConsumer Consume, FDiagnostic& Error) {
    const auto Read = [&](const FReadRequest& Request) { return Consume(Request, nullptr); };
    return Producer(Read, Error);
  };
  return CaptureSnapshotProduced(Borrow, INDEX_NONE, OutResult, nullptr, nullptr, ValueMode, nullptr, OutReadSeconds, OutRequestCount);
}

bool CaptureSnapshotOwned(FOwnedReadRequestProducer Producer, FCaptureResult& OutResult,
    ECaptureValueMode ValueMode, double* OutReadSeconds, int32* OutRequestCount) {
  const auto Transfer = [&](FInternalReadConsumer Consume, FDiagnostic& Error) {
    const auto Read = [&](FReadRequest&& Request) { return Consume(Request, &Request); };
    return Producer(Read, Error);
  };
  return CaptureSnapshotProduced(Transfer, INDEX_NONE, OutResult, nullptr, nullptr, ValueMode, nullptr, OutReadSeconds, OutRequestCount);
}

bool CaptureSnapshot(const TArray<FReadRequest>& Requests, FCaptureResult& OutResult,
    const UeAgentAuthoringSemanticRegistry::FRegistry* Semantics,
    const FStateAccessorRegistry* Accessors, ECaptureValueMode ValueMode) {
  return CaptureSnapshotImpl(Requests, OutResult, Semantics, Accessors, ValueMode, nullptr);
}
bool CaptureSnapshot(const TArray<FReadRequestBinding>& Requests, FCaptureResult& OutResult,
    const UeAgentAuthoringSemanticRegistry::FRegistry* Semantics,
    const FStateAccessorRegistry* Accessors, ECaptureValueMode ValueMode) {
  return CaptureSnapshotImpl(Requests, OutResult, Semantics, Accessors, ValueMode, nullptr);
}

bool CaptureSnapshotWithBaseline(const TArray<FReadRequest>& Requests, const FStateSnapshot& Baseline,
    FCaptureResult& OutResult) {
  return CaptureSnapshotImpl(Requests, OutResult, nullptr, nullptr, ECaptureValueMode::Authoring, &Baseline);
}
} // namespace UeAgentAuthoringState
