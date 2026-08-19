// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"

#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringReflectionDescriptors.h"
#include "Utilities/UeAgentPropertyValueCodec.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeRWLock.h"
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

bool ValidateAccessor(const FStateAccessorDescriptor &Descriptor,
                      const FStateAccessor &Accessor, FString &OutError) {
  if (Descriptor.AccessorId.IsEmpty())
    OutError = TEXT("state_accessor_id_missing");
  else if (Descriptor.TargetKinds.IsEmpty())
    OutError = TEXT("state_accessor_target_kinds_missing");
  else if (!Accessor)
    OutError = TEXT("state_accessor_callback_missing");
  else {
    OutError.Reset();
    return true;
  }
  return false;
}

TSharedPtr<FJsonValue> CloneJsonValue(const TSharedPtr<FJsonValue> &Value) {
  if (!Value.IsValid())
    return nullptr;

  auto MakeCloneShell = [](const TSharedPtr<FJsonValue> &Source) {
    if (!Source.IsValid())
      return TSharedPtr<FJsonValue>();
    switch (Source->Type) {
    case EJson::String:
      return TSharedPtr<FJsonValue>(
          MakeShared<FJsonValueString>(Source->AsString()));
    case EJson::Number:
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
    case EJson::Number:
      Out += TEXT("n") +
             FString::Printf(TEXT("%.17g"), Task.JsonValue->AsNumber());
      break;
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
                       bool bIgnoreFixedArray = false) {
  if (!Property || !Value.IsValid())
    return CloneJsonValue(Value);
  if (Property->ArrayDim > 1 && !bIgnoreFixedArray &&
      Value->Type == EJson::Object) {
    TSharedPtr<FJsonObject> Object = CloneJsonObject(Value->AsObject());
    const TArray<TSharedPtr<FJsonValue>> *SourceItems = nullptr;
    if (Object->TryGetArrayField(TEXT("items"), SourceItems) && SourceItems) {
      TArray<TSharedPtr<FJsonValue>> Items;
      for (const TSharedPtr<FJsonValue> &Item : *SourceItems) {
        Items.Add(NormalizePropertyValue(Property, Item, true));
      }
      Object->SetArrayField(TEXT("items"), Items);
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
          NormalizedFields->SetField(It->GetName(),
                                     NormalizePropertyValue(*It, FieldValue));
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
    TArray<TSharedPtr<FJsonValue>> Items;
    for (const TSharedPtr<FJsonValue> &Item : *SourceItems)
      Items.Add(NormalizePropertyValue(Array->Inner, Item));
    Object->SetArrayField(TEXT("items"), Items);
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
    TArray<TSharedPtr<FJsonValue>> Items;
    for (const TSharedPtr<FJsonValue> &Item : *SourceItems)
      Items.Add(NormalizePropertyValue(Set->ElementProp, Item));
    Items.Sort([](const TSharedPtr<FJsonValue> &Left,
                  const TSharedPtr<FJsonValue> &Right) {
      return StableJsonKey(Left) < StableJsonKey(Right);
    });
    Object->SetArrayField(TEXT("items"), Items);
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
    TArray<TSharedPtr<FJsonValue>> Entries;
    for (const TSharedPtr<FJsonValue> &SourceEntryValue : *SourceEntries) {
      if (!SourceEntryValue.IsValid() ||
          SourceEntryValue->Type != EJson::Object) {
        Entries.Add(CloneJsonValue(SourceEntryValue));
        continue;
      }
      TSharedPtr<FJsonObject> Entry =
          CloneJsonObject(SourceEntryValue->AsObject());
      Entry->SetField(TEXT("key"),
                      NormalizePropertyValue(Map->KeyProp,
                                             Entry->TryGetField(TEXT("key"))));
      Entry->SetField(TEXT("value"),
                      NormalizePropertyValue(
                          Map->ValueProp, Entry->TryGetField(TEXT("value"))));
      Entries.Add(MakeShared<FJsonValueObject>(Entry));
    }
    Entries.Sort([](const TSharedPtr<FJsonValue> &Left,
                    const TSharedPtr<FJsonValue> &Right) {
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
    Object->SetArrayField(TEXT("entries"), Entries);
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
      Object->SetField(TEXT("value"), NormalizePropertyValue(
                                          Optional->GetValueProperty(),
                                          Object->TryGetField(TEXT("value"))));
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
                                  FString &OutError) {
  OutValue.Reset();
  if (!Property || !ValuePtr) {
    OutError = TEXT("state_property_value_invalid");
    return false;
  }
  OutError.Reset();

  UeAgentPropertyValueCodec::FOptions CodecOptions;
	CodecOptions.MaxDepth = -1;
  CodecOptions.MaxContainerItems = Limits.MaxContainerItems;
  CodecOptions.MaxStringChars = Limits.MaxStringChars;
	CodecOptions.StructFieldFilter = Limits.StructFieldFilter;
  CodecOptions.ObjectReferenceEncoder =
      [&CodecOptions, &Limits, &ObjectStack, &OutError](
          const FObjectPropertyBase *ObjectProperty, const void *ObjectValuePtr,
          UObject *ObjectOwner, int32 ObjectDepth,
          TSharedPtr<FJsonValue> &OutEncoded) -> bool {
    if (!UeAgentAuthoringReflection::IsOwnedObjectProperty(ObjectProperty))
      return false;
    auto FailOwnedObject = [&OutEncoded, &OutError](FString Error) {
      OutError = MoveTemp(Error);
      OutEncoded = MakeShared<FJsonValueNull>();
      return true;
    };

    UObject *Referenced =
        ObjectProperty->GetObjectPropertyValue(ObjectValuePtr);
    if (!Referenced) {
      TSharedRef<FJsonObject> NullReference = MakeShared<FJsonObject>();
      NullReference->SetField(TEXT("path"), MakeShared<FJsonValueNull>());
      OutEncoded = MakeShared<FJsonValueObject>(NullReference);
      return true;
    }
    if (!ObjectOwner || !IsObjectWithin(Referenced, ObjectOwner)) {
      return FailOwnedObject(TEXT("state_owned_object_outside_owner:") +
                             Referenced->GetPathName());
    }
    if (ObjectStack.Contains(Referenced)) {
      return FailOwnedObject(TEXT("state_owned_object_cycle:") +
                             Referenced->GetPathName());
    }

    TArray<FProperty *> Properties;
    for (TFieldIterator<FProperty> It(Referenced->GetClass(),
                                      EFieldIterationFlags::IncludeSuper);
         It; ++It) {
      FProperty *NestedProperty = *It;
      const void *NestedValuePtr =
          NestedProperty
              ? NestedProperty->ContainerPtrToValuePtr<void>(Referenced)
              : nullptr;
      UeAgentAuthoringReflection::FDescriptor Reflection;
      FString ReflectionError;
      if (!NestedProperty || !UeAgentAuthoringReflection::Build(
                                 NestedProperty, Referenced, NestedValuePtr,
                                 Reflection, ReflectionError)) {
        return FailOwnedObject(
            ReflectionError.IsEmpty()
                ? FString(TEXT("state_owned_object_property_descriptor_failed"))
                : MoveTemp(ReflectionError));
      }
      if (Reflection.bEditable)
        Properties.Add(NestedProperty);
    }
    Properties.Sort([](const FProperty &Left, const FProperty &Right) {
      return Left.GetName() < Right.GetName();
    });
    if (Limits.MaxContainerItems > 0 &&
        Properties.Num() > Limits.MaxContainerItems) {
      return FailOwnedObject(FString::Printf(
          TEXT("state_owned_object_property_limit_exceeded:%s:%d"),
          *Referenced->GetPathName(), Properties.Num()));
    }

    ObjectStack.Add(Referenced);
    TArray<TSharedPtr<FJsonValue>> PropertyValues;
    PropertyValues.Reserve(Properties.Num());
    for (FProperty *NestedProperty : Properties) {
      TSharedPtr<FJsonValue> NestedValue = UeAgentPropertyValueCodec::Encode(
          NestedProperty,
          NestedProperty->ContainerPtrToValuePtr<void>(Referenced), Referenced,
          CodecOptions, ObjectDepth + 1);
      if (!OutError.IsEmpty()) {
        ObjectStack.Remove(Referenced);
        OutEncoded = MakeShared<FJsonValueNull>();
        return true;
      }
      FString ReliabilityReason;
      if (!UeAgentPropertyValueCodec::IsReliableCanonicalValue(
              NestedProperty, NestedValue, &ReliabilityReason)) {
        ObjectStack.Remove(Referenced);
        return FailOwnedObject(
            ReliabilityReason.IsEmpty()
                ? FString(TEXT("state_owned_object_property_value_unreliable"))
                : MoveTemp(ReliabilityReason));
      }
      TSharedRef<FJsonObject> Descriptor = MakeShared<FJsonObject>();
      Descriptor->SetStringField(TEXT("property_path"),
                                 NestedProperty->GetName());
      Descriptor->SetField(TEXT("value"),
                           NormalizePropertyValue(NestedProperty, NestedValue));
      PropertyValues.Add(MakeShared<FJsonValueObject>(Descriptor));
    }
    ObjectStack.Remove(Referenced);

    TSharedRef<FJsonObject> OwnedObject = MakeShared<FJsonObject>();
    OwnedObject->SetStringField(TEXT("class_path"),
                                Referenced->GetClass()->GetPathName());
    OwnedObject->SetArrayField(TEXT("properties"), PropertyValues);
    TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
    Value->SetObjectField(TEXT("owned_object"), OwnedObject);
    OutEncoded = MakeShared<FJsonValueObject>(Value);
    return true;
  };

  TSharedPtr<FJsonValue> Encoded = UeAgentPropertyValueCodec::Encode(
      Property, ValuePtr, Owner, CodecOptions, Depth);
  if (!OutError.IsEmpty())
    return false;
  FString ReliabilityReason;
  if (!UeAgentPropertyValueCodec::IsReliableCanonicalValue(
          Property, Encoded, &ReliabilityReason)) {
    OutError = ReliabilityReason.IsEmpty()
                   ? FString(TEXT("state_property_value_unreliable"))
                   : ReliabilityReason;
    return false;
  }
  OutValue = NormalizePropertyValue(Property, Encoded);
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
	Clone.CandidatesHeader = CloneJsonValue(Entry.CandidatesHeader);
	Clone.Candidates = CloneJsonValue(Entry.Candidates);
  return Clone;
}

EReadOutcome ReadPropertyState(const FReadRequest &Request,
                               FStateValue &OutValue, FString &OutError) {
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
    OutError = TargetResult.ErrorCode.IsEmpty()
                   ? FString(TEXT("state_property_target_not_found"))
                   : TargetResult.ErrorCode;
    return EReadOutcome::Failed;
  }
  if (Target.Identity.CanonicalKey != Request.TargetIdentity.CanonicalKey) {
    OutError = TEXT("state_accessor_identity_mismatch");
    return EReadOutcome::Failed;
  }

  UObject *Owner = Target.NotificationOwner
                       ? Target.NotificationOwner
                       : const_cast<UObject *>(Request.NotificationOwner
                                                   ? Request.NotificationOwner
                                                   : Request.RootObject);
  UeAgentAuthoringReflection::FDescriptor Reflection;
  if (!UeAgentAuthoringReflection::Build(
          Target.Property, Owner, Target.ValuePtr, Reflection, OutError)) {
    return EReadOutcome::Failed;
  }
  TSharedPtr<FJsonValue> Encoded;
  TSet<const UObject *> ObjectStack;
  if (Owner)
    ObjectStack.Add(Owner);
  if (!EncodeAuthoringPropertyValue(Target.Property, Target.ValuePtr, Owner,
                                    Request.Limits, 0, ObjectStack, Encoded,
                                    OutError)) {
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
  OutValue.Constraints = Reflection.Constraints;
	OutValue.DisplayName = Reflection.DisplayName;
	OutValue.Tooltip = Reflection.Tooltip;
	OutValue.CategoryPath = Reflection.CategoryPath;
	OutValue.PropertyFlags = Reflection.PropertyFlags;
	if (const UEnum* Enum = UeAgentAuthoringTypeDescriptors::GetPropertyEnum(Target.Property)) {
		TArray<TSharedPtr<FJsonValue>> Header;
		Header.Add(MakeShared<FJsonValueString>(TEXT("name")));
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (int32 Index = 0; Index < Enum->NumEnums(); ++Index) {
			if (Enum->HasMetaData(TEXT("Hidden"), Index)) continue;
			const FString Name = Enum->GetNameStringByIndex(Index);
			if (Name.IsEmpty() || Name.Equals(TEXT("MAX"), ESearchCase::IgnoreCase)
				|| Name.EndsWith(TEXT("_MAX"), ESearchCase::IgnoreCase)) continue;
			Rows.Add(MakeShared<FJsonValueArray>(
				TArray<TSharedPtr<FJsonValue>>{MakeShared<FJsonValueString>(Name)}));
		}
		if (!Rows.IsEmpty()) {
			OutValue.CandidatesHeader = MakeShared<FJsonValueArray>(MoveTemp(Header));
			OutValue.Candidates = MakeShared<FJsonValueArray>(MoveTemp(Rows));
		}
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
    OutResult.ErrorCode = TEXT("state_target_identity_invalid");
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
      OutResult.ErrorCode = RequestTargetResult.ErrorCode;
      return false;
    }
  }
  if (SemanticIdentity.ModelId != Semantic.IdentityModel) {
    OutResult.ErrorCode = TEXT("state_identity_model_mismatch");
    return false;
  }
  if (SemanticIdentity.TargetKind != Semantic.TargetKind) {
    OutResult.ErrorCode = TEXT("state_target_kind_mismatch");
    return false;
  }

  UeAgentAuthoringTargetLocator::FTargetQuery Query;
  Query.ModelId = SemanticIdentity.ModelId;
  Query.TargetKind = SemanticIdentity.TargetKind;
  Query.CanonicalKey = SemanticIdentity.CanonicalKey;
  UeAgentAuthoringTargetLocator::FResolveResult TargetResult;
  if (!UeAgentAuthoringTargetLocator::ResolveExact(Query, {SemanticIdentity},
                                                   TargetResult)) {
    OutResult.ErrorCode = TargetResult.ErrorCode;
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
  return AccessorId == Other.AccessorId && TargetKinds == Other.TargetKinds &&
         CapabilityTags == Other.CapabilityTags;
}

FStateAccessorRegistry &FStateAccessorRegistry::Get() {
  static FStateAccessorRegistry Registry;
  static const bool bBuiltInsRegistered = []() {
    FString Error;
    return RegisterBuiltInStateAccessors(Registry, Error);
  }();
  ensureAlwaysMsgf(bBuiltInsRegistered,
                   TEXT("State accessor built-in registration failed."));
  return Registry;
}

bool FStateAccessorRegistry::RegisterAccessor(
    const FStateAccessorDescriptor &Descriptor, FStateAccessor Accessor,
    ERegistrationResult &OutResult, FString &OutError) {
  const FStateAccessorDescriptor Canonical = Normalize(Descriptor);
  if (!ValidateAccessor(Canonical, Accessor, OutError))
    return false;
  FWriteScopeLock Lock(RegistryLock);
  if (const FEntry *Existing = Accessors.Find(Canonical.AccessorId)) {
    if (Existing->Descriptor != Canonical) {
      OutError = FString::Printf(TEXT("state_accessor_conflict_%s"),
                                 *Canonical.AccessorId);
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
  return !SemanticId.IsEmpty() && TargetIdentity.IsValid() &&
         !Type.Kind.IsEmpty() && !AccessorId.IsEmpty() &&
         (!bAvailable || Value.IsValid());
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
  return Status == TEXT("captured") &&
         ErrorCode == TEXT("state_snapshot_captured");
}

TSharedRef<FJsonObject> FCaptureResult::ToJsonObject() const {
  TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
  Object->SetStringField(TEXT("status"), Status);
  Object->SetStringField(TEXT("error_code"), ErrorCode);
  if (!ErrorDetail.IsEmpty())
    Object->SetStringField(TEXT("error_detail"), ErrorDetail);
  if (!SemanticId.IsEmpty())
    Object->SetStringField(TEXT("semantic_id"), SemanticId);
  if (!AccessorId.IsEmpty())
    Object->SetStringField(TEXT("accessor_id"), AccessorId);
  if (TargetIdentity.IsValid())
    Object->SetObjectField(TEXT("target_identity"),
                           TargetIdentity.ToJsonObject());
  if (IsCaptured())
    Object->SetObjectField(TEXT("snapshot"), Snapshot.ToJsonObject());
  else if (!ErrorCode.IsEmpty()) {
    UeAgentAuthoringDiagnostics::FDiagnostic Diagnostic;
    Diagnostic.Code = ErrorCode;
    Diagnostic.Stage = TEXT("state_snapshot");
    Diagnostic.Severity = UeAgentAuthoringDiagnostics::ESeverity::Error;
    Diagnostic.Message = ErrorDetail;
    Diagnostic.TargetPath = TargetIdentity.CanonicalKey;
    Diagnostic.PropertyPath = TargetIdentity.GetSelector(TEXT("property_path"));
    Diagnostic.SemanticId = SemanticId;
    Object->SetObjectField(TEXT("diagnostic"), Diagnostic.ToJson());
  }
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
                                   FString &OutError) {
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
    UeAgentAuthoringTargetLocator::FResolveResult &OutTargetResult) {
  OutRequest = FReadRequest();
  UeAgentAuthoringTargetLocator::FResolvedPropertyTarget Target;
  if (!UeAgentAuthoringTargetLocator::ResolvePropertyPath(
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
  OutRequest.RootOwnerPath = RootObject ? RootObject->GetPathName() : FString();
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

bool AppendEditableObjectPropertyReadRequests(
    const UObject *RootObject, const FEditablePropertyReadOptions &Options,
    TArray<FReadRequest> &OutRequests, FString &OutError) {
  OutError.Reset();
  if (!IsValid(RootObject) || Options.SemanticId.TrimStartAndEnd().IsEmpty()) {
    OutError = TEXT("editable_property_read_input_invalid");
    return false;
  }

  TArray<FProperty *> Properties;
  for (TFieldIterator<FProperty> It(RootObject->GetClass(),
                                    EFieldIteratorFlags::IncludeSuper);
       It; ++It) {
    FProperty *Property = *It;
    if (!Property ||
        Options.ExcludedProperties.Contains(Property->GetFName()) ||
        !UeAgentAuthoringReflection::IsEditableByFlags(Property, RootObject)) {
      continue;
    }
    const void *ValuePtr = Property->ContainerPtrToValuePtr<void>(RootObject);
	if (Options.IncludeProperty
		&& !Options.IncludeProperty(Property, ValuePtr, RootObject)) {
		continue;
	}
    FString SupportReason;
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
      OutError = TargetResult.ErrorCode.IsEmpty()
                     ? TEXT("editable_property_read_request_failed")
                     : TargetResult.ErrorCode;
      return false;
    }
    Request.Limits = Options.Limits;
    if (Options.BuildSnapshotIdentity) {
      UeAgentAuthoringIdentity::FCanonicalIdentity SnapshotIdentity;
      if (!Options.BuildSnapshotIdentity(Request.PropertyPath, SnapshotIdentity,
                                         OutError)) {
        if (OutError.IsEmpty())
          OutError = TEXT("editable_property_snapshot_identity_failed");
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
	TArray<FReadRequest> &OutRequests, FString &OutError) {
	OutError.Reset();
	if (!RootStruct || !RootData || Options.SemanticId.TrimStartAndEnd().IsEmpty()) {
		OutError = TEXT("editable_struct_property_read_input_invalid");
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
		FString ReflectionError;
		if (!UeAgentAuthoringReflection::Build(
				Property, const_cast<UObject *>(NotificationOwner), ValuePtr,
				Reflection, ReflectionError)
			|| !Reflection.bEditable) {
			continue;
		}
		if (Options.IncludeProperty
			&& !Options.IncludeProperty(Property, ValuePtr, NotificationOwner)) {
			continue;
		}
		FString SupportReason;
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
			OutError = TargetResult.ErrorCode.IsEmpty()
				? TEXT("editable_struct_property_read_request_failed")
				: TargetResult.ErrorCode;
			return false;
		}
		Request.Limits = Options.Limits;
		if (Options.BuildSnapshotIdentity) {
			UeAgentAuthoringIdentity::FCanonicalIdentity SnapshotIdentity;
			if (!Options.BuildSnapshotIdentity(
					Request.PropertyPath, SnapshotIdentity, OutError)) {
				if (OutError.IsEmpty())
					OutError = TEXT("editable_struct_property_snapshot_identity_failed");
				return false;
			}
			Request.SnapshotIdentityOverride = MoveTemp(SnapshotIdentity);
		}
		OutRequests.Add(MoveTemp(Request));
	}
	return true;
}

UObject *ResolveRequestRootObject(
	const FReadRequest &Request, FString &OutError) {
	OutError.Reset();
	if (Request.RootObject)
		return const_cast<UObject *>(Request.RootObject);
	FString Path = Request.TargetIdentity.GetSelector(TEXT("object_path"));
	if (Path.IsEmpty())
		Path = Request.TargetIdentity.GetSelector(TEXT("asset_path"));
	if (Path.IsEmpty()) {
		OutError = TEXT("state_request_object_path_missing");
		return nullptr;
	}
	if (!Path.Contains(TEXT("."))) {
		const FString Name = FPackageName::GetLongPackageAssetName(Path);
		if (!Name.IsEmpty())
			Path = FString::Printf(TEXT("%s.%s"), *Path, *Name);
	}
	UObject *Object = StaticLoadObject(UObject::StaticClass(), nullptr, *Path);
	if (!IsValid(Object)) {
		OutError = TEXT("state_request_object_not_found:") + Path;
		return nullptr;
	}
	return Object;
}

bool CaptureSnapshot(
    const TArray<FReadRequest> &Requests, FCaptureResult &OutResult,
    const UeAgentAuthoringSemanticRegistry::FRegistry *SemanticRegistry,
    const FStateAccessorRegistry *AccessorRegistry) {
  OutResult = FCaptureResult();
  if (!IsInGameThread()) {
    OutResult.Status = TEXT("failed");
    OutResult.ErrorCode = TEXT("state_snapshot_game_thread_required");
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
      OutResult.ErrorCode = TEXT("state_semantic_unknown");
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
      OutResult.ErrorCode = TEXT("state_accessor_unknown");
      OutResult.ErrorDetail = Semantic.StateAccessor;
      return false;
    }
    if (!AccessorDescriptor.SupportsTargetKind(
            Request.TargetIdentity.TargetKind)) {
      OutResult.Status = TEXT("failed");
      OutResult.ErrorCode = TEXT("state_accessor_target_kind_unsupported");
      return false;
    }
    FStateAccessor Accessor;
    if (!Accessors.CopyAccessor(Semantic.StateAccessor, Accessor)) {
      OutResult.Status = TEXT("failed");
      OutResult.ErrorCode = TEXT("state_accessor_unknown");
      return false;
    }

    FStateValue StateValue;
    FString AccessorError;
    const EReadOutcome Outcome = Accessor(Request, StateValue, AccessorError);
    if (Outcome == EReadOutcome::Failed) {
      OutResult.Status = TEXT("failed");
      OutResult.ErrorCode = TEXT("state_accessor_failed");
      OutResult.ErrorDetail = AccessorError;
      return false;
    }
    if (!StateValue.TargetIdentity.IsValid() ||
        StateValue.TargetIdentity.CanonicalKey !=
            Request.TargetIdentity.CanonicalKey) {
      OutResult.Status = TEXT("failed");
      OutResult.ErrorCode = TEXT("state_accessor_identity_mismatch");
      return false;
    }
    if (StateValue.Type.Kind.IsEmpty())
      StateValue.Type.Kind = Semantic.ValueType;
    if (Semantic.ValueType != TEXT("*") &&
        NormalizeId(StateValue.Type.Kind) != Semantic.ValueType) {
      OutResult.Status = TEXT("failed");
      OutResult.ErrorCode = TEXT("state_value_type_mismatch");
      OutResult.ErrorDetail = StateValue.Type.Kind;
      return false;
    }

    if (Request.SnapshotIdentityOverride.IsSet()) {
      const UeAgentAuthoringIdentity::FCanonicalIdentity &SnapshotIdentity =
          Request.SnapshotIdentityOverride.GetValue();
      if (!SnapshotIdentity.IsValid() ||
          SnapshotIdentity.ModelId != Semantic.IdentityModel ||
          SnapshotIdentity.TargetKind != Semantic.TargetKind) {
        OutResult.Status = TEXT("failed");
        OutResult.ErrorCode = TEXT("state_snapshot_identity_override_invalid");
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
    Entry.bEditable = Entry.bAvailable && StateValue.bEditable;
    Entry.AvailabilityReason = StateValue.AvailabilityReason;
	Entry.Constraints = StateValue.Constraints;
	Entry.DisplayName = StateValue.DisplayName;
	Entry.Tooltip = StateValue.Tooltip;
	Entry.CategoryPath = StateValue.CategoryPath;
	Entry.PropertyFlags = StateValue.PropertyFlags;
	Entry.CandidatesHeader = CloneJsonValue(StateValue.CandidatesHeader);
	Entry.Candidates = CloneJsonValue(StateValue.Candidates);
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
        OutResult.ErrorCode = TEXT("state_value_missing");
        return false;
      }
      Entry.Value =
          StateValue.NativeValue.IsValid()
              ? NormalizePropertyValue(StateValue.NativeValue->GetProperty(),
                                       StateValue.Value)
              : CloneJsonValue(StateValue.Value);
      if (StateValue.NativeValue.IsValid()) {
        FString StorageError;
        TSharedPtr<UeAgentAuthoringPropertyValues::FPropertyValueStorage>
            Detached =
                UeAgentAuthoringPropertyValues::FPropertyValueStorage::CopyFrom(
                    StateValue.NativeValue->GetProperty(),
                    StateValue.NativeValue->GetValuePtr(), StorageError);
        if (!Detached.IsValid()) {
          OutResult.Status = TEXT("failed");
          OutResult.ErrorCode = TEXT("state_native_value_copy_failed");
          OutResult.ErrorDetail = StorageError;
          return false;
        }
        Entry.NativeValue = Detached;
      }
    }
    if (!Entry.IsValid()) {
      OutResult.Status = TEXT("failed");
      OutResult.ErrorCode = TEXT("state_entry_invalid");
      return false;
    }
    const FString EntryKey =
        MakeEntryKey(Entry.SemanticId, Entry.TargetIdentity.CanonicalKey);
    if (EntryKeys.Contains(EntryKey)) {
      OutResult.Status = TEXT("failed");
      OutResult.ErrorCode = TEXT("state_snapshot_duplicate_entry");
      OutResult.ErrorDetail = EntryKey;
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
  OutResult.ErrorCode = TEXT("state_snapshot_captured");
  OutResult.ErrorDetail.Reset();
  OutResult.SemanticId.Reset();
  OutResult.AccessorId.Reset();
  OutResult.TargetIdentity = UeAgentAuthoringIdentity::FCanonicalIdentity();
  return true;
}
} // namespace UeAgentAuthoringState
