// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"

#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringEncodedTypes.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringReflectionDescriptors.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeRWLock.h"
#include "Misc/ScopeExit.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

namespace UeAgentAuthoringState {
namespace {
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

FString EscapeStableString(const FString &Value) {
  FString Result;
  Result.Reserve(Value.Len());
  for (const TCHAR Character : Value) {
    if (Character == TEXT('%') || Character == TEXT(':') ||
        Character == TEXT(';') || Character == TEXT('[') ||
        Character == TEXT(']') || Character == TEXT('{') ||
        Character == TEXT('}') || Character == TEXT(',') ||
        Character == TEXT('|') || Character == TEXT('\r') ||
        Character == TEXT('\n') || Character == TEXT('\t')) {
      Result += FString::Printf(TEXT("%%%04X"), static_cast<uint32>(Character));
    } else {
      Result.AppendChar(Character);
    }
  }
  return Result;
}

void AppendStableJson(const TSharedPtr<FJsonValue> &Value, FString &Out) {
  enum class EStableTaskType : uint8 {
    Value,
    Text,
    ExitObject,
    ExitArray,
  };
  struct FStableTask {
    EStableTaskType Type = EStableTaskType::Value;
    TSharedPtr<FJsonValue> JsonValue;
    FString Text;
    const FJsonObject *ObjectIdentity = nullptr;
    const TArray<TSharedPtr<FJsonValue>> *ArrayIdentity = nullptr;
  };

  TSet<const FJsonObject *> ActiveObjects;
  TSet<const TArray<TSharedPtr<FJsonValue>> *> ActiveArrays;
  TArray<FStableTask> Stack;
  Stack.Add({EStableTaskType::Value, Value});
  while (!Stack.IsEmpty()) {
    FStableTask Task = Stack.Pop(EAllowShrinking::No);
    if (Task.Type == EStableTaskType::Text) {
      Out += Task.Text;
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
      Out += TEXT("s:") + EscapeStableString(Task.JsonValue->AsString()) +
             TEXT(";");
      break;
    case EJson::Number: {
      FString Lexeme, Normalized;
      if (!UE::UAI::StateSemantic::CanonicalJson::TryGetNumberLexeme(*Task.JsonValue, Lexeme) ||
          !UE::UAI::StateSemantic::TryNormalizeNumberLexeme(Lexeme, Normalized)) {
        Out += TEXT("invalid-number");
        break;
      }
      // Retain the existing revision material only when its numeric spelling
      // is exact. NumberString values must not collapse through double.
      double LegacyNumber = 0.0;
      if (Task.JsonValue->TryGetNumber(LegacyNumber) && FMath::IsFinite(LegacyNumber)) {
        const FString Legacy = FString::Printf(TEXT("%.17g"), LegacyNumber == 0.0 ? 0.0 : LegacyNumber);
        FString LegacyNormalized;
        if (UE::UAI::StateSemantic::TryNormalizeNumberLexeme(Legacy, LegacyNormalized) &&
            LegacyNormalized == Normalized)
          Normalized = Legacy;
      }
      Out += TEXT("n") + Normalized;
      break;
    }
    case EJson::Boolean:
      Out += Task.JsonValue->AsBool() ? TEXT("bt") : TEXT("bf");
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
      TArray<FString> Keys;
      Identity->Values.GetKeys(Keys);
      Keys.Sort();
      for (int32 Index = Keys.Num() - 1; Index >= 0; --Index) {
        const FString &Key = Keys[Index];
        Stack.Add(
            {EStableTaskType::Value, Identity->TryGetField(Key)});
        FStableTask KeyTask;
        KeyTask.Type = EStableTaskType::Text;
        KeyTask.Text =
            TEXT("k:") + EscapeStableString(Key) + TEXT(";");
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

bool EncodeAuthoringPropertyValue(FProperty *Property, const void *ValuePtr,
                                  UObject *Owner, const FReadLimits &Limits,
                                  int32 Depth,
                                  TSet<const UObject *> &ObjectStack,
                                  TSharedPtr<FJsonValue> &OutValue,
                                  FDiagnostic& OutError,
                                  TSharedPtr<FJsonValue> &OutValueDefinition) {
  OutValue.Reset();
  OutValueDefinition.Reset();
  if (!Property || !ValuePtr) {
    OutError = MissingStateField(TEXT("state_property_value_invalid"), !Property ? TEXT("The property descriptor is missing.") : TEXT("The native property value storage is missing."), !Property ? TEXT("property") : Property->GetName());
    return false;
  }
  OutError.Reset();

  UeAgentPropertyValueCodec::FOptions CodecOptions;
  CodecOptions.bCaptureOwnedObjectDefinitions = true;
  CodecOptions.bCaptureStructFieldDescriptors = true;
	CodecOptions.StructFieldFilter = Limits.StructFieldFilter;
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
  FDiagnostic ReliabilityReason;
  if (!UeAgentPropertyValueCodec::IsReliableCanonicalValue(
          Property, Encoded, &ReliabilityReason)) {
    OutError = MoveTemp(ReliabilityReason);
    if (OutError.PropertyPath.IsEmpty()) OutError.PropertyPath = Property->GetName();
    return false;
  }
  if (Definition.IsValid()) OutValueDefinition = MakeShared<FJsonValueObject>(Definition);
  OutValue = NormalizePropertyValue(Property, Encoded, false, &OutValueDefinition);
  OutError.Reset();
  return true;
}

FString MakeEntryKey(const FString &SemanticId, const FString &CanonicalKey) {
  return FString::Printf(TEXT("%d:%s%s"), SemanticId.Len(), *SemanticId,
                         *CanonicalKey);
}

FStateEntry CloneStateEntry(const FStateEntry &Entry) {
  FStateEntry Clone = Entry;
  Clone.Value = CloneJsonValue(Entry.Value);
  Clone.ValueDefinition = CloneJsonValue(Entry.ValueDefinition);
  Clone.ComparisonValueDefinition = CloneJsonValue(Entry.ComparisonValueDefinition);
	Clone.CandidatesHeader = CloneJsonValue(Entry.CandidatesHeader);
	Clone.Candidates = CloneJsonValue(Entry.Candidates);
  return Clone;
}

EReadOutcome ReadPropertyState(const FReadRequest &Request,
                               FStateValue &OutValue, FDiagnostic& OutError) {
  UeAgentAuthoringTargetLocator::FResolvedPropertyTarget Target;
  UeAgentAuthoringTargetLocator::FResolveResult TargetResult;
  const bool bResolved =
      Request.RootObject
          ? UeAgentAuthoringTargetLocator::ResolvePropertyPath(
                const_cast<UObject *>(Request.RootObject), Request.PropertyPath,
                Target, TargetResult)
          : UeAgentAuthoringTargetLocator::ResolvePropertyPath(
                Request.RootStruct, const_cast<void *>(Request.RootData),
                Request.RootOwnerPath,
                const_cast<UObject *>(Request.NotificationOwner),
                Request.PropertyPath, Target, TargetResult);
  if (!bResolved) {
    OutError = MoveTemp(TargetResult.Diagnostic);
    return EReadOutcome::Failed;
  }
  if (Target.Identity.CanonicalKey != Request.TargetIdentity.CanonicalKey) {
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
  UeAgentAuthoringReflection::FDescriptor Reflection;
  if (!UeAgentAuthoringReflection::Build(
          Target.Property, Owner, Target.ValuePtr, Reflection, OutError, Request.Limits.bShowDefaultsOnly, Request.Limits.bUseInstanceEditability, Target.PropertyScope)) {
    return EReadOutcome::Failed;
  }
  TSharedPtr<FJsonValue> Encoded;
  TSet<const UObject *> ObjectStack;
  if (Owner)
    ObjectStack.Add(Owner);
  if (!EncodeAuthoringPropertyValue(Target.Property, Target.ValuePtr, Owner,
                                    Request.Limits, 0, ObjectStack, Encoded,
                                    OutError, OutValue.ValueDefinition)) {
    return EReadOutcome::Failed;
  }

  OutValue.TargetIdentity = Target.Identity;
  OutValue.Type = Reflection.Type;
  OutValue.Value = Encoded;
  OutValue.NativeValue =
      UeAgentAuthoringPropertyValues::FPropertyValueStorage::CopyFrom(
          Target.Property, Target.ValuePtr, OutError);
  if (!OutValue.NativeValue.IsValid())
    return EReadOutcome::Failed;
  OutValue.bAvailable = true;
  OutValue.bEditable = Reflection.bEditable;
  OutValue.AvailabilityReason = Reflection.AvailabilityReason;
  if (Request.Limits.AuthorizeProperty && Request.Limits.AuthorizeProperty(Target.Property)) {
    OutValue.bEditable = true;
    OutValue.AvailabilityReason.Reset();
  }
  OutValue.Constraints = Reflection.Constraints;
	OutValue.DisplayName = Reflection.DisplayName;
	OutValue.Tooltip = Reflection.Tooltip;
	OutValue.CategoryPath = Reflection.CategoryPath;
	OutValue.PropertyFlags = Reflection.PropertyFlags;
	if (const auto Candidates = UeAgentAuthoringTypeDescriptors::BuildEnumCandidates(UeAgentAuthoringTypeDescriptors::GetPropertyEnum(Target.Property), false, Target.Property)) {
		OutValue.CandidatesHeader = Candidates->TryGetField(TEXT("candidates_header"));
		OutValue.Candidates = Candidates->TryGetField(TEXT("candidates"));
	}

  Target.Property->ExportTextItem_Direct(OutValue.ValueText, Target.ValuePtr,
                                         nullptr, Owner, PPF_None);
  OutValue.Provenance = TEXT("shared_property_accessor");
  OutError.Reset();
  return EReadOutcome::Resolved;
}

bool ValidateTarget(
    const FReadRequest &Request,
    const UeAgentAuthoringSemanticRegistry::FSemanticDescriptor &Semantic,
    FCaptureResult &OutResult) {
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
  if (!Request.TargetQuery.ModelId.IsEmpty() ||
      !Request.TargetQuery.TargetKind.IsEmpty() ||
      !Request.TargetQuery.CanonicalKey.IsEmpty() ||
      !Request.TargetQuery.Selectors.IsEmpty()) {
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
    UeAgentAuthoringTargetLocator::FResolveResult RequestTargetResult;
    if (!UeAgentAuthoringTargetLocator::ResolveExact(
            CompletedQuery, {Request.TargetIdentity}, RequestTargetResult)) {
      OutResult.Diagnostic = RequestTargetResult.Diagnostic;
      return false;
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

  UeAgentAuthoringTargetLocator::FTargetQuery Query;
  Query.ModelId = SemanticIdentity.ModelId;
  Query.TargetKind = SemanticIdentity.TargetKind;
  Query.CanonicalKey = SemanticIdentity.CanonicalKey;
  UeAgentAuthoringTargetLocator::FResolveResult TargetResult;
  if (!UeAgentAuthoringTargetLocator::ResolveExact(Query, {SemanticIdentity},
                                                   TargetResult)) {
    OutResult.Diagnostic = TargetResult.Diagnostic;
    return false;
  }
  return true;
}
} // namespace

class FSnapshotBuilder {
public:
  static void SetEntries(FStateSnapshot &Snapshot,
                         TArray<FStateEntry> &&Entries) {
    Snapshot.Entries = MoveTemp(Entries);
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
  FReadScopeLock Lock(RegistryLock);
  if (const FEntry *Entry = Accessors.Find(NormalizeId(AccessorId))) {
    OutAccessor = Entry->Accessor;
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
  else if (Type.Kind.IsEmpty()) Error = MissingStateField(TEXT("state_entry_invalid"), TEXT("The state entry has no declared value type."), TEXT("type.kind"));
  else if (AccessorId.IsEmpty()) Error = MissingStateField(TEXT("state_entry_invalid"), TEXT("The state entry has no accessor identifier."), TEXT("accessor_id"));
  else if (bAvailable && !Value.IsValid()) Error = MissingStateField(TEXT("state_entry_invalid"), TEXT("An available state entry must contain a value."), TEXT("value"));
  if (!Error.IsEmpty()) Error.SemanticId = SemanticId;
  return Error;
}

TSharedRef<FJsonObject> FStateEntry::ToJsonObject() const {
  TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
  Object->SetStringField(TEXT("semantic_id"), SemanticId);
  Object->SetObjectField(TEXT("target_identity"),
                         TargetIdentity.ToJsonObject());
  if (TSharedPtr<FJsonObject> TypeObject = Type.ToJsonObject();
      TypeObject.IsValid()) {
    Object->SetObjectField(TEXT("type"), TypeObject);
  }
  TSharedRef<FJsonObject> Availability = MakeShared<FJsonObject>();
  Availability->SetBoolField(TEXT("available"), bAvailable);
  Availability->SetBoolField(TEXT("editable"), bEditable);
  if (!AvailabilityReason.IsEmpty())
    Availability->SetStringField(TEXT("reason"), AvailabilityReason);
  Object->SetObjectField(TEXT("availability"), Availability);
  if (!Constraints.IsEmpty()) {
    TSharedRef<FJsonObject> ConstraintObject = MakeShared<FJsonObject>();
    for (const TPair<FString, FString> &Pair : Constraints)
      ConstraintObject->SetStringField(Pair.Key, Pair.Value);
    Object->SetObjectField(TEXT("constraints"), ConstraintObject);
  }
	if (!DisplayName.IsEmpty())
		Object->SetStringField(TEXT("display_name"), DisplayName);
	if (!Tooltip.IsEmpty())
		Object->SetStringField(TEXT("tooltip"), Tooltip);
	if (!CategoryPath.IsEmpty()) {
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString &Part : CategoryPath)
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
  if (bAvailable && Value.IsValid())
    Object->SetField(TEXT("value"), CloneJsonValue(Value));
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

bool FStateSnapshot::CopyEntry(int32 Index, FStateEntry &OutEntry) const {
  if (!Entries.IsValidIndex(Index))
    return false;
  OutEntry = CloneStateEntry(Entries[Index]);
  return true;
}

bool FStateSnapshot::FindEntry(const FString &SemanticId,
                               const FString &TargetCanonicalKey,
                               FStateEntry &OutEntry) const {
  const FString CanonicalSemanticId = NormalizeId(SemanticId);
  const FStateEntry *Entry =
      Entries.FindByPredicate([&CanonicalSemanticId, &TargetCanonicalKey](
                                  const FStateEntry &Candidate) {
        return Candidate.SemanticId == CanonicalSemanticId &&
               Candidate.TargetIdentity.CanonicalKey == TargetCanonicalKey;
      });
  if (!Entry)
    return false;
  OutEntry = CloneStateEntry(*Entry);
  return true;
}

TArray<FStateEntry> FStateSnapshot::CopyEntries() const {
  TArray<FStateEntry> Result;
  Result.Reserve(Entries.Num());
  for (const FStateEntry &Entry : Entries)
    Result.Add(CloneStateEntry(Entry));
  return Result;
}

void FStateSnapshot::ReleaseNativeValues() {
  for (FStateEntry &Entry : Entries) {
    Entry.NativeValue.Reset();
    Entry.NativeReadLimits.Reset();
  }
}

TSharedRef<FJsonObject> FStateSnapshot::ToJsonObject() const {
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

FString MakeStableCanonicalValueKey(const TSharedPtr<FJsonValue> &Value) {
  return StableJsonKey(Value);
}

bool RegisterBuiltInStateAccessors(FStateAccessorRegistry &Registry,
                                   UE::UAI::Contract::FDiagnostic& OutError) {
  FStateAccessorDescriptor Descriptor;
  Descriptor.AccessorId = TEXT("core.property");
  Descriptor.TargetKinds = {TEXT("property")};
  Descriptor.CapabilityTags = {TEXT("canonical_value"), TEXT("readonly"),
                               TEXT("reflection")};
  ERegistrationResult Result = ERegistrationResult::Registered;
  return Registry.RegisterAccessor(Descriptor, ReadPropertyState, Result,
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
  Result.StructFieldFilter = [Filter = Limits.StructFieldFilter, Authorize = Limits.AuthorizeProperty](const UScriptStruct* Struct, const FProperty* Field) {
    return (UeAgentPropertyValueCodec::IsStructFieldAuthoringEditable(Field, Struct) || (Authorize && Authorize(Field)))
        && (!Filter || Filter(Struct, Field));
  };
  return Result;
}

bool AppendEditableObjectPropertyReadRequests(
    const UObject *RootObject, const FEditablePropertyReadOptions &Options,
    TArray<FReadRequest> &OutRequests, FDiagnostic& OutError) {
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

  TArray<FProperty *> Properties;
  for (TFieldIterator<FProperty> It(RootObject->GetClass(),
                                    EFieldIteratorFlags::IncludeSuper);
       It; ++It) {
    FProperty *Property = *It;
    if (!Property ||
        Options.ExcludedProperties.Contains(Property->GetFName()) ||
        (!UeAgentAuthoringReflection::IsEditableByFlags(
             Property, RootObject, Options.Limits.bShowDefaultsOnly, Options.Limits.bUseInstanceEditability)
         && !(Options.Limits.AuthorizeProperty && Options.Limits.AuthorizeProperty(Property))
         && !UeAgentAuthoringReflection::IsExistingInlineObjectProperty(
             Property, Property->ContainerPtrToValuePtr<void>(RootObject),
             RootObject))) {
      continue;
    }
    const void *ValuePtr = Property->ContainerPtrToValuePtr<void>(RootObject);
	if (Options.IncludeProperty
		&& !Options.IncludeProperty(Property, ValuePtr, RootObject)) {
		continue;
	}
    FDiagnostic SupportReason;
    if (!UeAgentPropertyValueCodec::IsAuthoringSupported(Property, ValuePtr,
                                                         SupportReason))
      continue;
    Properties.Add(Property);
  }
  Properties.Sort([](const FProperty &Left, const FProperty &Right) {
    return Left.GetName() < Right.GetName();
  });

  for (FProperty *Property : Properties) {
    FReadRequest Request;
    UeAgentAuthoringTargetLocator::FResolveResult TargetResult;
    if (!BuildPropertyReadRequest(RootObject, Property->GetName(),
                                  Options.SemanticId, Request, TargetResult)) {
      OutError = TargetResult.Diagnostic;
      if (OutError.IsEmpty()) {
        OutError = FDiagnostic::Failure(TEXT("editable_property_read_request_failed"),
            TEXT("The editable object property could not be resolved for snapshot capture."),
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
    OutRequests.Add(MoveTemp(Request));
  }
  return true;
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

bool CaptureSnapshot(
    const TArray<FReadRequest> &Requests, FCaptureResult &OutResult,
    const UeAgentAuthoringSemanticRegistry::FRegistry *SemanticRegistry,
    const FStateAccessorRegistry *AccessorRegistry, ECaptureValueMode ValueMode) {
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
  const FStateAccessorRegistry &Accessors =
      AccessorRegistry ? *AccessorRegistry : FStateAccessorRegistry::Get();
  TArray<FStateEntry> CapturedEntries;
  TSet<FString> EntryKeys;
  for (const FReadRequest &Request : Requests) {
    const FString SemanticId = NormalizeId(Request.SemanticId);
    OutResult.SemanticId = SemanticId;
    OutResult.TargetIdentity = Request.TargetIdentity;
    UeAgentAuthoringSemanticRegistry::FSemanticDescriptor Semantic;
    if (!Semantics.FindSemantic(SemanticId, Semantic)) {
      OutResult.Status = TEXT("failed");
      OutResult.Diagnostic = StateFailure(TEXT("state_semantic_unknown"), TEXT("The requested state semantic is not registered."), TEXT("semantic_id"), DescriptorValue(FString(TEXT("registered semantic"))), DescriptorValue(SemanticId));
      return false;
    }
    if (!ValidateTarget(Request, Semantic, OutResult)) {
      OutResult.Status = TEXT("failed");
      return false;
    }

    FStateAccessorDescriptor AccessorDescriptor;
    OutResult.AccessorId = Semantic.StateAccessor;
    if (!Accessors.FindAccessor(Semantic.StateAccessor, AccessorDescriptor)) {
      OutResult.Status = TEXT("failed");
      OutResult.Diagnostic = StateFailure(TEXT("state_accessor_unknown"), TEXT("The semantic state accessor is not registered or has no callable implementation."), TEXT("accessor_id"), DescriptorValue(FString(TEXT("registered callable accessor"))), DescriptorValue(Semantic.StateAccessor));
      OutResult.Diagnostic.InternalContext = Semantic.StateAccessor;
      return false;
    }
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
    if (!Accessors.CopyAccessor(Semantic.StateAccessor, Accessor)) {
      OutResult.Status = TEXT("failed");
      OutResult.Diagnostic = StateFailure(TEXT("state_accessor_unknown"), TEXT("The semantic state accessor is not registered or has no callable implementation."), TEXT("accessor_id"), DescriptorValue(FString(TEXT("registered callable accessor"))), DescriptorValue(Semantic.StateAccessor));
      return false;
    }

    FStateValue StateValue;
    TSharedPtr<FJsonValue> ComparisonValueDefinition;
    FDiagnostic AccessorError;
    EReadOutcome Outcome;
    {
      UeAgentAuthoringEncodedTypes::FScope EncodedTypes;
      Outcome = Accessor(Request, StateValue, AccessorError);
      if (Outcome == EReadOutcome::Resolved)
      {
        if (ValueMode == ECaptureValueMode::Authoring)
          StateValue.Value = EncodedTypes.ProjectAuthoringValue(StateValue.Value);
        if (auto Definition = EncodedTypes.BuildDefinition(StateValue.Value))
        {
          // A document may declare only its collection contract. Keep native
          // leaf provenance too, without duplicating a complete codec definition.
          if (!StateValue.ValueDefinition || StateValue.ValueDefinition->Type != EJson::Object
              || StateValue.ValueDefinition->AsObject() != Definition)
            ComparisonValueDefinition = MakeShared<FJsonValueObject>(Definition);
        }
      }
    }
    if (Outcome == EReadOutcome::Failed) {
      OutResult.Status = TEXT("failed");
      OutResult.Diagnostic = AccessorError.IsEmpty()
          ? StateFailure(TEXT("state_accessor_failed"), TEXT("The state accessor reported failure without a diagnostic."), TEXT("accessor_id"), nullptr, DescriptorValue(Semantic.StateAccessor))
          : FDiagnostic::WrapFailure(TEXT("state_accessor_failed"), AccessorError, TEXT("state_snapshot"));
      return false;
    }
    if (!StateValue.TargetIdentity.IsValid() ||
        StateValue.TargetIdentity.CanonicalKey !=
            Request.TargetIdentity.CanonicalKey) {
      OutResult.Status = TEXT("failed");
      const auto IdentityError = StateValue.TargetIdentity.GetValidationDiagnostic();
      OutResult.Diagnostic = !IdentityError.IsEmpty()
          ? FDiagnostic::WrapFailure(TEXT("state_accessor_identity_mismatch"), IdentityError, TEXT("state_snapshot"))
          : StateFailure(TEXT("state_accessor_identity_mismatch"), TEXT("The state accessor returned a different target from the requested target."), TEXT("target_identity"));
      OutResult.Diagnostic.ExpectedEvidence = DescriptorValue(Request.TargetIdentity.CanonicalKey);
      OutResult.Diagnostic.ActualEvidence = DescriptorValue(StateValue.TargetIdentity.CanonicalKey);
      OutResult.Diagnostic.SourceKind = TEXT("native_result");
      return false;
    }
    if (StateValue.Type.Kind.IsEmpty())
      StateValue.Type.Kind = Semantic.ValueType;
    if (Semantic.ValueType != TEXT("*") &&
        NormalizeId(StateValue.Type.Kind) != Semantic.ValueType) {
      OutResult.Status = TEXT("failed");
      OutResult.Diagnostic = StateFailure(TEXT("state_value_type_mismatch"), TEXT("The captured value type does not match the declared semantic type."), TEXT("type.kind"), DescriptorValue(Semantic.ValueType), DescriptorValue(StateValue.Type.Kind));
      OutResult.Diagnostic.InternalContext = StateValue.Type.Kind;
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
    Entry.SemanticId = SemanticId;
    Entry.TargetIdentity = Request.SnapshotIdentityOverride.IsSet()
                               ? Request.SnapshotIdentityOverride.GetValue()
                               : StateValue.TargetIdentity;
    Entry.Type = StateValue.Type;
    Entry.bAvailable =
        Outcome == EReadOutcome::Resolved && StateValue.bAvailable;
    Entry.bEditable = Entry.bAvailable && StateValue.bEditable.Get(
        Semantic.WriteCapability == UeAgentAuthoringSemanticRegistry::EWriteCapability::DomainMutation);
    Entry.bAuthoringVisible = StateValue.bAuthoringVisible;
    Entry.AvailabilityReason = StateValue.AvailabilityReason;
	Entry.Constraints = StateValue.Constraints;
	Entry.DisplayName = StateValue.DisplayName;
	Entry.Tooltip = StateValue.Tooltip;
	Entry.CategoryPath = StateValue.CategoryPath;
	Entry.PropertyFlags = StateValue.PropertyFlags;
	Entry.CandidatesHeader = CloneJsonValue(StateValue.CandidatesHeader);
	Entry.Candidates = CloneJsonValue(StateValue.Candidates);
	Entry.ValueDefinition = CloneJsonValue(StateValue.ValueDefinition);
	Entry.ComparisonValueDefinition = CloneJsonValue(ComparisonValueDefinition);
    Entry.AccessorId = AccessorDescriptor.AccessorId;
    Entry.ValueText = StateValue.ValueText;
    Entry.Provenance = StateValue.Provenance;
    if (Outcome == EReadOutcome::Unavailable) {
      Entry.bAvailable = false;
      Entry.bEditable = false;
      if (Entry.AvailabilityReason.IsEmpty())
        Entry.AvailabilityReason = TEXT("state_unavailable");
    } else {
      if (!StateValue.Value.IsValid()) {
        OutResult.Status = TEXT("failed");
        OutResult.Diagnostic = MissingStateField(TEXT("state_value_missing"), TEXT("The state accessor reported a resolved value but returned no value storage."), TEXT("value"));
        return false;
      }
      Entry.Value =
          StateValue.NativeValue.IsValid()
              ? NormalizePropertyValue(StateValue.NativeValue->GetProperty(),
                                       StateValue.Value)
              : CloneJsonValue(StateValue.Value);
      if (StateValue.NativeValue.IsValid()) {
        FDiagnostic StorageError;
        TSharedPtr<UeAgentAuthoringPropertyValues::FPropertyValueStorage>
            Detached =
                UeAgentAuthoringPropertyValues::FPropertyValueStorage::CopyFrom(
                    StateValue.NativeValue->GetProperty(),
                    StateValue.NativeValue->GetValuePtr(), StorageError);
        if (!Detached.IsValid()) {
          OutResult.Status = TEXT("failed");
          OutResult.Diagnostic = FDiagnostic::WrapFailure(TEXT("state_native_value_copy_failed"), StorageError, TEXT("state_snapshot"));
          return false;
        }
        Entry.NativeValue = Detached;
        Entry.NativeTargetIdentity = StateValue.TargetIdentity;
        Entry.NativeReadLimits = MakeShared<FReadLimits>(Request.Limits);
      }
    }
    if (!Entry.IsValid()) {
      OutResult.Status = TEXT("failed");
      OutResult.Diagnostic = Entry.GetValidationDiagnostic();
      return false;
    }
    const FString EntryKey =
        MakeEntryKey(Entry.SemanticId, Entry.TargetIdentity.CanonicalKey);
    if (EntryKeys.Contains(EntryKey)) {
      OutResult.Status = TEXT("failed");
      OutResult.Diagnostic = StateFailure(TEXT("state_snapshot_duplicate_entry"), TEXT("The snapshot contains the same semantic and target more than once."), Entry.TargetIdentity.GetSelector(TEXT("property_path")));
      OutResult.Diagnostic.InternalContext = EntryKey;
      OutResult.SemanticId = Entry.SemanticId;
      OutResult.AccessorId = Entry.AccessorId;
      OutResult.TargetIdentity = Entry.TargetIdentity;
      return false;
    }
    EntryKeys.Add(EntryKey);
    CapturedEntries.Add(CloneStateEntry(Entry));
  }

  CapturedEntries.Sort([](const FStateEntry &Left, const FStateEntry &Right) {
    if (Left.SemanticId != Right.SemanticId)
      return Left.SemanticId < Right.SemanticId;
    return Left.TargetIdentity.CanonicalKey < Right.TargetIdentity.CanonicalKey;
  });
  FSnapshotBuilder::SetEntries(OutResult.Snapshot, MoveTemp(CapturedEntries));
  OutResult.Status = TEXT("captured");
  OutResult.Diagnostic.Reset();
  OutResult.SemanticId.Reset();
  OutResult.AccessorId.Reset();
  OutResult.TargetIdentity = UeAgentAuthoringIdentity::FCanonicalIdentity();
  return true;
}
} // namespace UeAgentAuthoringState
