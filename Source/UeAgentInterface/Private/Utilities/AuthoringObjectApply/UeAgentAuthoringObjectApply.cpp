// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringObjectApply/UeAgentAuthoringObjectApply.h"

#include "Utilities/AuthoringCore/UeAgentAuthoringNativeDefaults.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringPropertyValues.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringReflectionDescriptors.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticCompare.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTargetLocator.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTypeDescriptors.h"
#include "Utilities/UeAgentPropertyValueCodec.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "HAL/CriticalSection.h"
#include "IDetailTreeNode.h"
#include "IPropertyRowGenerator.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "PropertyHandle.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/StructOnScope.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

namespace UeAgentAuthoringObjectApply::Private {
using FPropertyValueStorage =
    UeAgentAuthoringPropertyValues::FPropertyValueStorage;

struct FMaterialization {
  TWeakObjectPtr<UObject> ScratchObject;
  TStrongObjectPtr<UObject> CreatedObject;
};

struct FPreparedProperty {
  FString PropertyPath;
  UObject *NotificationOwner = nullptr;
  FProperty *Property = nullptr;
  void *CurrentValuePtr = nullptr;
  TSharedPtr<FPropertyValueStorage> DesiredValue;
  TSharedPtr<FPropertyValueStorage> OriginalValue;
  FResolvedNode Node;
  TArray<TUniquePtr<FPreparedApply>> NestedOwnedPlans;
  TArray<FMaterialization> Materializations;
  int32 NestedPlansAttempted = 0;
  bool bPropertyApplied = false;
};

struct FPrepareSession {
  TSet<const UObject *> ObjectStack;
};
} // namespace UeAgentAuthoringObjectApply::Private

namespace UeAgentAuthoringObjectApply {
struct FPreparedApply::FImpl {
  UStruct *RootStruct = nullptr;
  void *RootData = nullptr;
  UObject *RootObject = nullptr;
  UObject *NotificationOwner = nullptr;
  FString RootOwnerPath;
  FOptions Options;
  TArray<Private::FPreparedProperty> Properties;
  TArray<FResolvedNode> Nodes;
  UeAgentAuthoringNativeDefaults::FScratchScope Scratch;
  UPackage *ScratchPackage = nullptr;
  TMap<UPackage *, bool> PackageDirtyStates;
  int32 PropertiesAttempted = 0;
  bool bReady = false;
  bool bApplied = false;
  bool bRolledBack = false;
};

struct FPreparedApplyAccess {
  static FPreparedApply::FImpl &Get(FPreparedApply &Prepared) {
    return *Prepared.Impl;
  }
};
} // namespace UeAgentAuthoringObjectApply

namespace {
FCriticalSection GDomainOwnerAuthorizationMutex;
TMap<UeAgentAuthoringObjectApply::FDomainOwnerAuthorizationHandle, FString>
    GDomainOwnerAuthorizationPrefixes;
UeAgentAuthoringObjectApply::FDomainOwnerAuthorizationHandle
    GNextDomainOwnerAuthorizationHandle = 1;

bool IsRegisteredDomainAuthorizedOwnerPackage(const FString &PackageName) {
  FScopeLock Lock(&GDomainOwnerAuthorizationMutex);
  for (const TPair<UeAgentAuthoringObjectApply::FDomainOwnerAuthorizationHandle,
                   FString> &Entry : GDomainOwnerAuthorizationPrefixes) {
    if (PackageName.StartsWith(Entry.Value, ESearchCase::IgnoreCase)) {
      return true;
    }
  }
  return false;
}

using namespace UeAgentAuthoringObjectApply;
using namespace UeAgentAuthoringObjectApply::Private;
using FPropertyValueStorage =
    UeAgentAuthoringPropertyValues::FPropertyValueStorage;

bool PrepareRootInternal(
    UStruct *RootStruct, void *RootData, UObject *RootObject,
    UObject *NotificationOwner, const FString &RootOwnerPath,
    const TArray<TSharedPtr<FJsonValue>> &PropertyDescriptors,
    const FOptions &Options, FPrepareSession &Session,
    FPreparedApply &OutPrepared, FFailure &OutFailure);

void SetFailure(FFailure &OutFailure, const FString &ErrorCode,
                const FString &ErrorDetail,
                const FString &PropertyPath = FString()) {
  OutFailure = FFailure();
  OutFailure.ErrorCode = ErrorCode;
  OutFailure.ErrorDetail = ErrorDetail.IsEmpty() ? ErrorCode : ErrorDetail;
  OutFailure.PropertyPath = PropertyPath;
  OutFailure.Diagnostic.Code = ErrorCode;
  OutFailure.Diagnostic.Stage = TEXT("object_apply_preflight");
  OutFailure.Diagnostic.Severity =
      UeAgentAuthoringDiagnostics::ESeverity::Error;
  OutFailure.Diagnostic.bRetrySafe = true;
  OutFailure.Diagnostic.Message = OutFailure.ErrorDetail;
  OutFailure.Diagnostic.PropertyPath = PropertyPath;
}

const TSet<FString> &PropertyDescriptorFields() {
  static const TSet<FString> Fields = {
      TEXT("property_path"), TEXT("value"),        TEXT("value_kind"),
      TEXT("struct_path"),   TEXT("enum_path"),    TEXT("element_type"),
      TEXT("key_type"),      TEXT("mapped_type"),  TEXT("inner_type"),
      TEXT("fixed_length"),  TEXT("allowed_class")};
  return Fields;
}

const TSet<FString> &PropertyDescriptorPresentationFields() {
  static const TSet<FString> Fields = {
      TEXT("display_name"),   TEXT("tooltip"),       TEXT("availability"),
      TEXT("constraints"),    TEXT("candidates"),    TEXT("candidates_header"),
      TEXT("candidates_ref"), TEXT("category_path"), TEXT("value_text")};
  return Fields;
}

TSharedPtr<FJsonValue>
ProjectValueForApply(const TSharedPtr<FJsonValue> &Value) {
  if (!Value.IsValid())
    return nullptr;
  switch (Value->Type) {
  case EJson::None:
  case EJson::Null:
    return MakeShared<FJsonValueNull>();
  case EJson::String:
    return MakeShared<FJsonValueString>(Value->AsString());
  case EJson::Number:
    return MakeShared<FJsonValueNumber>(Value->AsNumber());
  case EJson::Boolean:
    return MakeShared<FJsonValueBoolean>(Value->AsBool());
  case EJson::Array: {
    TArray<TSharedPtr<FJsonValue>> Items;
    Items.Reserve(Value->AsArray().Num());
    for (const TSharedPtr<FJsonValue> &Item : Value->AsArray()) {
      Items.Add(ProjectValueForApply(Item));
    }
    return MakeShared<FJsonValueArray>(MoveTemp(Items));
  }
  case EJson::Object: {
    const TSharedPtr<FJsonObject> Object = Value->AsObject();
    if (!Object.IsValid())
      return nullptr;
    const bool bPropertyDescriptor =
        Object->HasTypedField<EJson::String>(TEXT("property_path")) &&
        Object->HasField(TEXT("value"));
    TSharedRef<FJsonObject> Projected = MakeShared<FJsonObject>();
    for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair : Object->Values) {
      if (bPropertyDescriptor &&
          PropertyDescriptorPresentationFields().Contains(Pair.Key))
        continue;
      Projected->SetField(Pair.Key, ProjectValueForApply(Pair.Value));
    }
    return MakeShared<FJsonValueObject>(Projected);
  }
  default:
    return nullptr;
  }
}

bool IsDangerousProperty(const FProperty *Property) {
  return !Property ||
         Property->HasAnyPropertyFlags(
             CPF_Transient | CPF_Deprecated | CPF_DuplicateTransient |
             CPF_NonPIEDuplicateTransient | CPF_TextExportTransient);
}

bool ValidateRootOwner(const UObject *Object, const FOptions &Options,
                       FString &OutReason) {
  OutReason.Reset();
  if (!Object || !Object->GetClass() || !Object->GetPackage()) {
    OutReason = TEXT("invalid_owner");
    return false;
  }
  if (!Options.bAllowTemplateOwners &&
      Object->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject)) {
    OutReason = TEXT("template_owner_requires_domain_authorization");
    return false;
  }
  if (const UWorld *World = Object->GetWorld();
      World && World->WorldType == EWorldType::PIE) {
    OutReason = TEXT("pie_runtime_owner_requires_runtime_command");
    return false;
  }
  if (Object->GetPackage() == GetTransientPackage() ||
      Object->HasAnyFlags(RF_Transient))
    return true;
  const FString PackageName = Object->GetPackage()->GetName();
  if (PackageName == TEXT("/Game") || PackageName.StartsWith(TEXT("/Game/")))
    return true;
  if ((Options.bAllowDomainAuthorizedOwners ||
       IsRegisteredDomainAuthorizedOwnerPackage(PackageName)) &&
      !PackageName.StartsWith(TEXT("/Engine/")))
    return true;
  OutReason = PackageName.StartsWith(TEXT("/Engine/"))
                  ? TEXT("engine_content_owner_is_read_only")
                  : TEXT("non_game_mount_owner_requires_domain_authorization");
  return false;
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

bool IsEditableForApply(const FProperty *Property, const UObject *Owner,
                        const FOptions &Options, FString &OutReason) {
  if (IsDangerousProperty(Property)) {
    OutReason = TEXT("transient_deprecated_or_unsafe_property");
    return false;
  }
  if (!Options.bAllowReadOnly &&
      !UeAgentAuthoringReflection::IsEditableByFlags(Property, Owner)) {
    OutReason = TEXT("read_only_or_non_editable");
    return false;
  }
  return true;
}

FString PropertyTypePath(const FProperty *Property) {
  if (const FStructProperty *Struct = CastField<FStructProperty>(Property)) {
    return Struct->Struct ? Struct->Struct->GetPathName() : FString();
  }
  if (const FObjectPropertyBase *ObjectProperty =
          CastField<FObjectPropertyBase>(Property)) {
    return ObjectProperty->PropertyClass
               ? ObjectProperty->PropertyClass->GetPathName()
               : FString();
  }
  return Property ? Property->GetCPPType() : FString();
}

TSharedPtr<FJsonValue> MakeObjectPathValue(const UObject *Object) {
  TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
  if (Object)
    Value->SetStringField(TEXT("path"), Object->GetPathName());
  else
    Value->SetField(TEXT("path"), MakeShared<FJsonValueNull>());
  return MakeShared<FJsonValueObject>(Value);
}

bool GetStrictObject(const TSharedPtr<FJsonValue> &Value,
                     TSharedPtr<FJsonObject> &OutObject,
                     const FString &ErrorCode, FFailure &OutFailure,
                     const FString &PropertyPath) {
  OutObject = Value.IsValid() && Value->Type == EJson::Object
                  ? Value->AsObject()
                  : nullptr;
  if (OutObject.IsValid())
    return true;
  SetFailure(OutFailure, ErrorCode, ErrorCode, PropertyPath);
  return false;
}

bool HasOnlyFields(const TSharedPtr<FJsonObject> &Object,
                   std::initializer_list<const TCHAR *> AllowedFields,
                   FFailure &OutFailure, const FString &PropertyPath,
                   const FString &ErrorPrefix) {
  TSet<FString> Allowed;
  for (const TCHAR *Field : AllowedFields)
    Allowed.Add(Field);
  TArray<FString> Actual;
  Object->Values.GetKeys(Actual);
  Actual.Sort();
  for (const FString &Field : Actual) {
    if (Allowed.Contains(Field))
      continue;
    SetFailure(OutFailure, ErrorPrefix, Field, PropertyPath);
    return false;
  }
  return true;
}

bool ValidateContainerCount(const FOptions &Options, int32 Count,
                            FFailure &OutFailure, const FString &PropertyPath) {
  if (Count >= 0 &&
      (Options.MaxContainerItems <= 0 || Count <= Options.MaxContainerItems))
    return true;
  SetFailure(OutFailure, TEXT("container_item_limit_exceeded"),
             LexToString(Count), PropertyPath);
  return false;
}

UeAgentPropertyValueCodec::FOptions MakeCodecOptions(const FOptions &Options) {
  UeAgentPropertyValueCodec::FOptions Result;
  Result.MaxDepth = -1;
  Result.MaxContainerItems = Options.MaxContainerItems;
  Result.MaxStringChars = Options.MaxStringChars;
  return Result;
}

bool BuildCompletedValue(FProperty *Property,
                         const TSharedPtr<FJsonValue> &InputValue,
                         const void *BaseValuePtr, UObject *Owner,
                         FPreparedApply::FImpl &Prepared,
                         FPreparedProperty &Entry, const FOptions &Options,
                         bool bIgnoreFixedArray, bool bHashKeyContext,
                         FPrepareSession &Session, const FString &PropertyPath,
                         TSharedPtr<FJsonValue> &OutCompletedValue,
                         FResolvedNode &OutNode, FFailure &OutFailure);

bool BuildOwnedObjectValue(FObjectPropertyBase *ObjectProperty,
                           const TSharedPtr<FJsonValue> &InputValue,
                           const void *BaseValuePtr, UObject *Owner,
                           FPreparedApply::FImpl &Prepared,
                           FPreparedProperty &Entry, const FOptions &Options,
                           bool bHashKeyContext, FPrepareSession &Session,
                           const FString &PropertyPath,
                           TSharedPtr<FJsonValue> &OutCompletedValue,
                           FResolvedNode &OutNode, FFailure &OutFailure) {
  TSharedPtr<FJsonObject> InputObject;
  if (!GetStrictObject(InputValue, InputObject,
                       TEXT("owned_object_value_invalid"), OutFailure,
                       PropertyPath))
    return false;
  const TSharedPtr<FJsonObject> *OwnedObject = nullptr;
  if (!InputObject->TryGetObjectField(TEXT("owned_object"), OwnedObject) ||
      !OwnedObject || !OwnedObject->IsValid()) {
    OutCompletedValue = InputValue;
    return true;
  }
  if (bHashKeyContext) {
    SetFailure(OutFailure, TEXT("owned_object_hash_key_forbidden"),
               PropertyPath, PropertyPath);
    return false;
  }
  if (InputObject->Values.Num() != 1 ||
      !HasOnlyFields(*OwnedObject, {TEXT("class_path"), TEXT("properties")},
                     OutFailure, PropertyPath,
                     TEXT("owned_object_unknown_field"))) {
    return false;
  }

  FString ExplicitClassPath;
  (*OwnedObject)->TryGetStringField(TEXT("class_path"), ExplicitClassPath);
  const TArray<TSharedPtr<FJsonValue>> *NestedProperties = nullptr;
  if ((*OwnedObject)->HasField(TEXT("properties")) &&
      (!(*OwnedObject)
            ->TryGetArrayField(TEXT("properties"), NestedProperties) ||
       !NestedProperties)) {
    SetFailure(OutFailure, TEXT("owned_object_properties_invalid"),
               PropertyPath, PropertyPath);
    return false;
  }
  static const TArray<TSharedPtr<FJsonValue>> EmptyProperties;
  const TArray<TSharedPtr<FJsonValue>> &Properties =
      NestedProperties ? *NestedProperties : EmptyProperties;

  UObject *ExistingObject =
      ObjectProperty->GetObjectPropertyValue(BaseValuePtr);
  UClass *ObjectClass = ExistingObject ? ExistingObject->GetClass() : nullptr;
  if (!ExplicitClassPath.IsEmpty()) {
    UClass *ExplicitClass = LoadObject<UClass>(nullptr, *ExplicitClassPath);
    if (!ExplicitClass) {
      SetFailure(OutFailure, TEXT("owned_object_class_not_found"),
                 ExplicitClassPath, PropertyPath);
      return false;
    }
    if (!ObjectProperty->PropertyClass ||
        !ExplicitClass->IsChildOf(ObjectProperty->PropertyClass)) {
      SetFailure(OutFailure, TEXT("owned_object_class_not_allowed"),
                 ExplicitClassPath, PropertyPath);
      return false;
    }
    if (ExistingObject && ExistingObject->GetClass() != ExplicitClass) {
      SetFailure(OutFailure, TEXT("owned_object_existing_class_conflict"),
                 ExplicitClassPath, PropertyPath);
      return false;
    }
    ObjectClass = ExplicitClass;
  } else if (!ExistingObject) {
    UClass *DeclaredClass = ObjectProperty->PropertyClass.Get();
    if (!DeclaredClass ||
        DeclaredClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated |
                                        CLASS_NewerVersionExists)) {
      SetFailure(OutFailure, TEXT("owned_object_class_source_required"),
                 PropertyPath, PropertyPath);
      return false;
    }
    ObjectClass = DeclaredClass;
  }
  if (!ObjectClass ||
      ObjectClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated |
                                    CLASS_NewerVersionExists)) {
    SetFailure(OutFailure, TEXT("owned_object_class_invalid"), PropertyPath,
               PropertyPath);
    return false;
  }

  OutNode.ValueKind = TEXT("owned_object");
  OutNode.TypePath = ObjectClass->GetPathName();
  if (ExistingObject) {
    if (!Owner || !IsObjectWithin(ExistingObject, Owner)) {
      SetFailure(OutFailure, TEXT("owned_object_outer_invalid"),
                 ExistingObject->GetPathName(), PropertyPath);
      return false;
    }
    if (Session.ObjectStack.Contains(ExistingObject)) {
      SetFailure(OutFailure, TEXT("object_identity_cycle"),
                 ExistingObject->GetPathName(), PropertyPath);
      return false;
    }
    if (!Properties.IsEmpty()) {
      TUniquePtr<FPreparedApply> NestedPlan = MakeUnique<FPreparedApply>();
      if (!PrepareRootInternal(ExistingObject->GetClass(), ExistingObject,
                               ExistingObject, ExistingObject,
                               ExistingObject->GetPathName(), Properties,
                               Options, Session, *NestedPlan, OutFailure)) {
        OutFailure.PropertyPath =
            PropertyPath + TEXT(".") + OutFailure.PropertyPath;
        OutFailure.Diagnostic.PropertyPath = OutFailure.PropertyPath;
        return false;
      }
      OutNode.Children = NestedPlan->GetResolvedNodes();
      if (NestedPlan->NumProperties() > 0) {
        Entry.NestedOwnedPlans.Add(MoveTemp(NestedPlan));
      }
    }
    OutCompletedValue = MakeObjectPathValue(ExistingObject);
    return true;
  }

  FString ScratchError;
  UPackage *ScratchPackage = Prepared.ScratchPackage;
  if (!ScratchPackage) {
    ScratchPackage = Prepared.Scratch.CreateScratchPackage(ScratchError);
    Prepared.ScratchPackage = ScratchPackage;
  }
  if (!ScratchPackage) {
    SetFailure(OutFailure, ScratchError, ScratchError, PropertyPath);
    return false;
  }
  UObject *ScratchObject = Prepared.Scratch.CreateObject(
      ObjectClass, ScratchPackage, nullptr, ScratchError);
  if (!ScratchObject) {
    SetFailure(OutFailure, ScratchError, ScratchError, PropertyPath);
    return false;
  }

  if (!Properties.IsEmpty()) {
    FOptions ScratchOptions = Options;
    ScratchOptions.bEmitEditorNotifications = false;
    ScratchOptions.bMarkPackageDirty = false;
    ScratchOptions.bAllowTemplateOwners = true;
    FPreparedApply ScratchPlan;
    if (!PrepareRootInternal(
            ScratchObject->GetClass(), ScratchObject, ScratchObject,
            ScratchObject, ScratchObject->GetPathName(), Properties,
            ScratchOptions, Session, ScratchPlan, OutFailure)) {
      return false;
    }
    FApplyResult ScratchResult;
    if (!ApplyPrepared(ScratchPlan, ScratchResult) ||
        !VerifyPrepared(ScratchPlan, ScratchResult)) {
      SetFailure(OutFailure,
                 ScratchResult.ErrorCode.IsEmpty()
                     ? TEXT("owned_object_scratch_apply_failed")
                     : ScratchResult.ErrorCode,
                 ScratchResult.ErrorDetail, PropertyPath);
      return false;
    }
    OutNode.Children = ScratchPlan.GetResolvedNodes();
  }

  FMaterialization &Materialization =
      Entry.Materializations.AddDefaulted_GetRef();
  Materialization.ScratchObject = ScratchObject;
  OutCompletedValue = MakeObjectPathValue(ScratchObject);
  return true;
}

bool BuildCompletedValue(FProperty *Property,
                         const TSharedPtr<FJsonValue> &InputValue,
                         const void *BaseValuePtr, UObject *Owner,
                         FPreparedApply::FImpl &Prepared,
                         FPreparedProperty &Entry, const FOptions &Options,
                         bool bIgnoreFixedArray, bool bHashKeyContext,
                         FPrepareSession &Session, const FString &PropertyPath,
                         TSharedPtr<FJsonValue> &OutCompletedValue,
                         FResolvedNode &OutNode, FFailure &OutFailure) {
  OutCompletedValue.Reset();
  OutNode = FResolvedNode();
  OutNode.PropertyPath = PropertyPath;
  OutNode.ValueKind = UeAgentAuthoringTypeDescriptors::GetValueKind(
      Property, bIgnoreFixedArray);
  OutNode.TypePath = PropertyTypePath(Property);
  if (!Property || !InputValue.IsValid()) {
    SetFailure(OutFailure, TEXT("invalid_apply_value"), PropertyPath,
               PropertyPath);
    return false;
  }
  if (!bIgnoreFixedArray && Property->ArrayDim > 1) {
    TSharedPtr<FJsonObject> Object;
    if (!GetStrictObject(InputValue, Object, TEXT("fixed_array_value_invalid"),
                         OutFailure, PropertyPath) ||
        !HasOnlyFields(Object, {TEXT("items")}, OutFailure, PropertyPath,
                       TEXT("fixed_array_unknown_field")))
      return false;
    const TArray<TSharedPtr<FJsonValue>> *Items = nullptr;
    if (!Object->TryGetArrayField(TEXT("items"), Items) || !Items ||
        Items->Num() != Property->ArrayDim ||
        !ValidateContainerCount(Options, Items ? Items->Num() : -1, OutFailure,
                                PropertyPath)) {
      if (OutFailure.ErrorCode.IsEmpty())
        SetFailure(OutFailure, TEXT("fixed_array_length_mismatch"),
                   PropertyPath, PropertyPath);
      return false;
    }
    TArray<TSharedPtr<FJsonValue>> CompletedItems;
    for (int32 Index = 0; Index < Items->Num(); ++Index) {
      const void *ElementBase = BaseValuePtr
                                    ? static_cast<const uint8 *>(BaseValuePtr) +
                                          Index * Property->GetElementSize()
                                    : nullptr;
      TSharedPtr<FJsonValue> Completed;
      FResolvedNode Child;
      if (!BuildCompletedValue(
              Property, (*Items)[Index], ElementBase, Owner, Prepared, Entry,
              Options, true, bHashKeyContext, Session,
              FString::Printf(TEXT("%s[%d]"), *PropertyPath, Index), Completed,
              Child, OutFailure))
        return false;
      CompletedItems.Add(Completed);
      OutNode.Children.Add(MoveTemp(Child));
    }
    TSharedRef<FJsonObject> CompletedObject = MakeShared<FJsonObject>();
    CompletedObject->SetArrayField(TEXT("items"), CompletedItems);
    OutCompletedValue = MakeShared<FJsonValueObject>(CompletedObject);
    return true;
  }

  if (FObjectPropertyBase *ObjectProperty =
          CastField<FObjectPropertyBase>(Property)) {
    TSharedPtr<FJsonObject> Object =
        InputValue->Type == EJson::Object ? InputValue->AsObject() : nullptr;
    const bool bRequestsRecursiveOwned =
        Object.IsValid() && Object->HasField(TEXT("owned_object"));
    if (bRequestsRecursiveOwned &&
        !UeAgentAuthoringReflection::IsOwnedObjectProperty(Property)) {
      SetFailure(OutFailure,
                 TEXT("external_reference_recursive_patch_forbidden"),
                 PropertyPath, PropertyPath);
      return false;
    }
    if (bRequestsRecursiveOwned) {
      return BuildOwnedObjectValue(ObjectProperty, InputValue, BaseValuePtr,
                                   Owner, Prepared, Entry, Options,
                                   bHashKeyContext, Session, PropertyPath,
                                   OutCompletedValue, OutNode, OutFailure);
    }
  }

  if (FStructProperty *StructProperty = CastField<FStructProperty>(Property)) {
    const FString StructKind =
        UeAgentAuthoringTypeDescriptors::GetStructKind(StructProperty->Struct);
    if (StructKind == TEXT("struct")) {
      TSharedPtr<FJsonObject> InputObject;
      if (!GetStrictObject(InputValue, InputObject,
                           TEXT("struct_value_invalid"), OutFailure,
                           PropertyPath) ||
          !HasOnlyFields(InputObject, {TEXT("fields")}, OutFailure,
                         PropertyPath, TEXT("struct_unknown_field")))
        return false;
      const TSharedPtr<FJsonObject> *InputFields = nullptr;
      if (!InputObject->TryGetObjectField(TEXT("fields"), InputFields) ||
          !InputFields || !InputFields->IsValid()) {
        SetFailure(OutFailure, TEXT("missing_struct_fields"), PropertyPath,
                   PropertyPath);
        return false;
      }

      TSharedPtr<FPropertyValueStorage> DefaultStorage;
      if (!BaseValuePtr) {
        FString StorageError;
        DefaultStorage =
            FPropertyValueStorage::CreateInitialized(Property, StorageError);
        if (!DefaultStorage.IsValid()) {
          SetFailure(OutFailure, StorageError, StorageError, PropertyPath);
          return false;
        }
        BaseValuePtr = DefaultStorage->GetValuePtr();
      }
      TSharedPtr<FJsonValue> BaseEncoded = UeAgentPropertyValueCodec::Encode(
          Property, BaseValuePtr, Owner, MakeCodecOptions(Options));
      TSharedPtr<FJsonObject> CompletedObject =
          BaseEncoded.IsValid() && BaseEncoded->Type == EJson::Object
              ? BaseEncoded->AsObject()
              : nullptr;
      const TSharedPtr<FJsonObject> *CompletedFieldsPtr = nullptr;
      if (!CompletedObject.IsValid() ||
          !CompletedObject->TryGetObjectField(TEXT("fields"),
                                              CompletedFieldsPtr) ||
          !CompletedFieldsPtr || !CompletedFieldsPtr->IsValid()) {
        SetFailure(OutFailure, TEXT("struct_base_encode_failed"), PropertyPath,
                   PropertyPath);
        return false;
      }
      TSharedPtr<FJsonObject> CompletedFields = *CompletedFieldsPtr;
      TArray<FString> FieldNames;
      (*InputFields)->Values.GetKeys(FieldNames);
      FieldNames.Sort();
      for (const FString &FieldName : FieldNames) {
        FProperty *Field =
            FindFProperty<FProperty>(StructProperty->Struct, *FieldName);
        FString FieldReason;
        if (!Field || !IsEditableForApply(Field, Owner, Options, FieldReason)) {
          SetFailure(OutFailure,
                     Field ? FieldReason : TEXT("unknown_struct_field"),
                     FieldName, PropertyPath + TEXT(".") + FieldName);
          return false;
        }
        TSharedPtr<FJsonValue> CompletedField;
        FResolvedNode Child;
        if (!BuildCompletedValue(
                Field, (*InputFields)->TryGetField(FieldName),
                Field->ContainerPtrToValuePtr<void>(BaseValuePtr), Owner,
                Prepared, Entry, Options, false, bHashKeyContext, Session,
                PropertyPath + TEXT(".") + FieldName, CompletedField, Child,
                OutFailure))
          return false;
        CompletedFields->SetField(FieldName, CompletedField);
        OutNode.Children.Add(MoveTemp(Child));
      }
      OutCompletedValue =
          MakeShared<FJsonValueObject>(CompletedObject.ToSharedRef());
      return true;
    }
    if (StructKind == TEXT("instanced_struct")) {
      TSharedPtr<FJsonObject> InputObject;
      if (!GetStrictObject(InputValue, InputObject,
                           TEXT("instanced_struct_value_invalid"), OutFailure,
                           PropertyPath) ||
          !HasOnlyFields(InputObject, {TEXT("struct_path"), TEXT("fields")},
                         OutFailure, PropertyPath,
                         TEXT("instanced_struct_unknown_field")))
        return false;
      FString StructPath;
      const TSharedPtr<FJsonValue> StructPathValue =
          InputObject->TryGetField(TEXT("struct_path"));
      if (!StructPathValue.IsValid() ||
          StructPathValue->Type != EJson::String) {
        OutCompletedValue = InputValue;
        return true;
      }
      StructPath = StructPathValue->AsString();
      UScriptStruct *DynamicStruct =
          LoadObject<UScriptStruct>(nullptr, *StructPath);
      const TSharedPtr<FJsonObject> *InputFields = nullptr;
      if (!DynamicStruct ||
          !InputObject->TryGetObjectField(TEXT("fields"), InputFields) ||
          !InputFields || !InputFields->IsValid()) {
        SetFailure(OutFailure, TEXT("instanced_struct_type_or_fields_invalid"),
                   StructPath, PropertyPath);
        return false;
      }
      const FInstancedStruct *Existing =
          static_cast<const FInstancedStruct *>(BaseValuePtr);
      FInstancedStruct DefaultDynamic;
      const void *DynamicBase = nullptr;
      if (Existing && Existing->GetScriptStruct() == DynamicStruct)
        DynamicBase = Existing->GetMemory();
      else {
        DefaultDynamic.InitializeAs(DynamicStruct);
        DynamicBase = DefaultDynamic.GetMemory();
      }
      TSharedRef<FJsonObject> CompletedFields = MakeShared<FJsonObject>();
      for (TFieldIterator<FProperty> It(DynamicStruct,
                                        EFieldIterationFlags::IncludeSuper);
           It; ++It) {
        FProperty *Field = *It;
        if (IsDangerousProperty(Field))
          continue;
        const void *FieldBase =
            Field->ContainerPtrToValuePtr<void>(DynamicBase);
        const TSharedPtr<FJsonValue> ExplicitField =
            (*InputFields)->TryGetField(Field->GetName());
        if (!ExplicitField.IsValid()) {
          CompletedFields->SetField(
              Field->GetName(),
              UeAgentPropertyValueCodec::Encode(Field, FieldBase, Owner,
                                                MakeCodecOptions(Options)));
          continue;
        }
        FString FieldReason;
        if (!IsEditableForApply(Field, Owner, Options, FieldReason)) {
          SetFailure(OutFailure, FieldReason, Field->GetName(),
                     PropertyPath + TEXT(".") + Field->GetName());
          return false;
        }
        TSharedPtr<FJsonValue> CompletedField;
        FResolvedNode Child;
        if (!BuildCompletedValue(Field, ExplicitField, FieldBase, Owner,
                                 Prepared, Entry, Options, false,
                                 bHashKeyContext, Session,
                                 PropertyPath + TEXT(".") + Field->GetName(),
                                 CompletedField, Child, OutFailure))
          return false;
        CompletedFields->SetField(Field->GetName(), CompletedField);
        OutNode.Children.Add(MoveTemp(Child));
      }
      for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair :
           (*InputFields)->Values) {
        if (!FindFProperty<FProperty>(DynamicStruct, *Pair.Key)) {
          SetFailure(OutFailure, TEXT("unknown_instanced_struct_field"),
                     Pair.Key, PropertyPath + TEXT(".") + Pair.Key);
          return false;
        }
      }
      TSharedRef<FJsonObject> CompletedObject = MakeShared<FJsonObject>();
      CompletedObject->SetStringField(TEXT("struct_path"), StructPath);
      CompletedObject->SetObjectField(TEXT("fields"), CompletedFields);
      OutCompletedValue = MakeShared<FJsonValueObject>(CompletedObject);
      return true;
    }
  }

  if (FArrayProperty *ArrayProperty = CastField<FArrayProperty>(Property)) {
    TSharedPtr<FJsonObject> InputObject;
    if (!GetStrictObject(InputValue, InputObject, TEXT("array_value_invalid"),
                         OutFailure, PropertyPath) ||
        !HasOnlyFields(InputObject, {TEXT("items")}, OutFailure, PropertyPath,
                       TEXT("array_unknown_field")))
      return false;
    const TArray<TSharedPtr<FJsonValue>> *Items = nullptr;
    if (!InputObject->TryGetArrayField(TEXT("items"), Items) || !Items ||
        !ValidateContainerCount(Options, Items ? Items->Num() : -1, OutFailure,
                                PropertyPath))
      return false;
    TUniquePtr<FScriptArrayHelper> BaseHelper;
    if (BaseValuePtr)
      BaseHelper = MakeUnique<FScriptArrayHelper>(ArrayProperty, BaseValuePtr);
    TArray<TSharedPtr<FJsonValue>> CompletedItems;
    for (int32 Index = 0; Index < Items->Num(); ++Index) {
      TSharedPtr<FPropertyValueStorage> DefaultValue;
      const void *ElementBase =
          BaseHelper.IsValid() && Index < BaseHelper->Num()
              ? BaseHelper->GetRawPtr(Index)
              : nullptr;
      if (!ElementBase) {
        FString StorageError;
        DefaultValue = FPropertyValueStorage::CreateInitialized(
            ArrayProperty->Inner, StorageError);
        if (!DefaultValue.IsValid()) {
          SetFailure(OutFailure, StorageError, StorageError, PropertyPath);
          return false;
        }
        ElementBase = DefaultValue->GetValuePtr();
      }
      TSharedPtr<FJsonValue> Completed;
      FResolvedNode Child;
      if (!BuildCompletedValue(
              ArrayProperty->Inner, (*Items)[Index], ElementBase, Owner,
              Prepared, Entry, Options, false, false, Session,
              FString::Printf(TEXT("%s[%d]"), *PropertyPath, Index), Completed,
              Child, OutFailure))
        return false;
      CompletedItems.Add(Completed);
      OutNode.Children.Add(MoveTemp(Child));
    }
    TSharedRef<FJsonObject> CompletedObject = MakeShared<FJsonObject>();
    CompletedObject->SetArrayField(TEXT("items"), CompletedItems);
    OutCompletedValue = MakeShared<FJsonValueObject>(CompletedObject);
    return true;
  }

  if (FSetProperty *SetProperty = CastField<FSetProperty>(Property)) {
    TSharedPtr<FJsonObject> InputObject;
    if (!GetStrictObject(InputValue, InputObject, TEXT("set_value_invalid"),
                         OutFailure, PropertyPath) ||
        !HasOnlyFields(InputObject, {TEXT("items")}, OutFailure, PropertyPath,
                       TEXT("set_unknown_field")))
      return false;
    const TArray<TSharedPtr<FJsonValue>> *Items = nullptr;
    if (!InputObject->TryGetArrayField(TEXT("items"), Items) || !Items ||
        !ValidateContainerCount(Options, Items ? Items->Num() : -1, OutFailure,
                                PropertyPath))
      return false;
    TArray<TSharedPtr<FJsonValue>> CompletedItems;
    for (int32 Index = 0; Index < Items->Num(); ++Index) {
      FString StorageError;
      TSharedPtr<FPropertyValueStorage> DefaultValue =
          FPropertyValueStorage::CreateInitialized(SetProperty->ElementProp,
                                                   StorageError);
      if (!DefaultValue.IsValid()) {
        SetFailure(OutFailure, StorageError, StorageError, PropertyPath);
        return false;
      }
      TSharedPtr<FJsonValue> Completed;
      FResolvedNode Child;
      if (!BuildCompletedValue(
              SetProperty->ElementProp, (*Items)[Index],
              DefaultValue->GetValuePtr(), Owner, Prepared, Entry, Options,
              false, true, Session,
              FString::Printf(TEXT("%s{%d}"), *PropertyPath, Index), Completed,
              Child, OutFailure))
        return false;
      CompletedItems.Add(Completed);
      OutNode.Children.Add(MoveTemp(Child));
    }
    TSharedRef<FJsonObject> CompletedObject = MakeShared<FJsonObject>();
    CompletedObject->SetArrayField(TEXT("items"), CompletedItems);
    OutCompletedValue = MakeShared<FJsonValueObject>(CompletedObject);
    return true;
  }

  if (FMapProperty *MapProperty = CastField<FMapProperty>(Property)) {
    TSharedPtr<FJsonObject> InputObject;
    if (!GetStrictObject(InputValue, InputObject, TEXT("map_value_invalid"),
                         OutFailure, PropertyPath) ||
        !HasOnlyFields(InputObject, {TEXT("entries")}, OutFailure, PropertyPath,
                       TEXT("map_unknown_field")))
      return false;
    const TArray<TSharedPtr<FJsonValue>> *Entries = nullptr;
    if (!InputObject->TryGetArrayField(TEXT("entries"), Entries) || !Entries ||
        !ValidateContainerCount(Options, Entries ? Entries->Num() : -1,
                                OutFailure, PropertyPath))
      return false;
    TArray<TSharedPtr<FJsonValue>> CompletedEntries;
    for (int32 Index = 0; Index < Entries->Num(); ++Index) {
      TSharedPtr<FJsonObject> EntryObject;
      if (!GetStrictObject((*Entries)[Index], EntryObject,
                           TEXT("map_entry_invalid"), OutFailure,
                           PropertyPath) ||
          !HasOnlyFields(EntryObject, {TEXT("key"), TEXT("value")}, OutFailure,
                         PropertyPath, TEXT("map_entry_unknown_field")))
        return false;
      const TSharedPtr<FJsonValue> KeyInput =
          EntryObject->TryGetField(TEXT("key"));
      const TSharedPtr<FJsonValue> MappedInput =
          EntryObject->TryGetField(TEXT("value"));
      if (!KeyInput.IsValid() || !MappedInput.IsValid()) {
        SetFailure(OutFailure, TEXT("map_entry_key_or_value_missing"),
                   LexToString(Index), PropertyPath);
        return false;
      }
      FString StorageError;
      TSharedPtr<FPropertyValueStorage> KeyDefault =
          FPropertyValueStorage::CreateInitialized(MapProperty->KeyProp,
                                                   StorageError);
      TSharedPtr<FPropertyValueStorage> ValueDefault =
          FPropertyValueStorage::CreateInitialized(MapProperty->ValueProp,
                                                   StorageError);
      if (!KeyDefault.IsValid() || !ValueDefault.IsValid()) {
        SetFailure(OutFailure, StorageError, StorageError, PropertyPath);
        return false;
      }
      TSharedPtr<FJsonValue> CompletedKey;
      TSharedPtr<FJsonValue> CompletedMapped;
      FResolvedNode KeyNode;
      FResolvedNode ValueNode;
      if (!BuildCompletedValue(
              MapProperty->KeyProp, KeyInput, KeyDefault->GetValuePtr(), Owner,
              Prepared, Entry, Options, false, true, Session,
              FString::Printf(TEXT("%s[%d].key"), *PropertyPath, Index),
              CompletedKey, KeyNode, OutFailure) ||
          !BuildCompletedValue(
              MapProperty->ValueProp, MappedInput, ValueDefault->GetValuePtr(),
              Owner, Prepared, Entry, Options, false, false, Session,
              FString::Printf(TEXT("%s[%d].value"), *PropertyPath, Index),
              CompletedMapped, ValueNode, OutFailure))
        return false;
      TSharedRef<FJsonObject> CompletedEntry = MakeShared<FJsonObject>();
      CompletedEntry->SetField(TEXT("key"), CompletedKey);
      CompletedEntry->SetField(TEXT("value"), CompletedMapped);
      CompletedEntries.Add(MakeShared<FJsonValueObject>(CompletedEntry));
      OutNode.Children.Add(MoveTemp(KeyNode));
      OutNode.Children.Add(MoveTemp(ValueNode));
    }
    TSharedRef<FJsonObject> CompletedObject = MakeShared<FJsonObject>();
    CompletedObject->SetArrayField(TEXT("entries"), CompletedEntries);
    OutCompletedValue = MakeShared<FJsonValueObject>(CompletedObject);
    return true;
  }

  if (FOptionalProperty *OptionalProperty =
          CastField<FOptionalProperty>(Property)) {
    TSharedPtr<FJsonObject> InputObject;
    if (!GetStrictObject(InputValue, InputObject,
                         TEXT("optional_value_invalid"), OutFailure,
                         PropertyPath) ||
        !HasOnlyFields(InputObject, {TEXT("is_set"), TEXT("value")}, OutFailure,
                       PropertyPath, TEXT("optional_unknown_field")))
      return false;
    bool bIsSet = false;
    if (!InputObject->TryGetBoolField(TEXT("is_set"), bIsSet)) {
      SetFailure(OutFailure, TEXT("optional_is_set_missing"), PropertyPath,
                 PropertyPath);
      return false;
    }
    if (!bIsSet) {
      if (InputObject->Values.Num() != 1) {
        SetFailure(OutFailure, TEXT("optional_unset_has_value"), PropertyPath,
                   PropertyPath);
        return false;
      }
      OutCompletedValue = InputValue;
      return true;
    }
    const TSharedPtr<FJsonValue> InnerInput =
        InputObject->TryGetField(TEXT("value"));
    if (!InnerInput.IsValid()) {
      SetFailure(OutFailure, TEXT("optional_value_missing"), PropertyPath,
                 PropertyPath);
      return false;
    }
    FString StorageError;
    TSharedPtr<FPropertyValueStorage> InnerDefault;
    const void *InnerBase =
        BaseValuePtr && OptionalProperty->IsSet(BaseValuePtr)
            ? OptionalProperty->GetValuePointerForRead(BaseValuePtr)
            : nullptr;
    if (!InnerBase) {
      InnerDefault = FPropertyValueStorage::CreateInitialized(
          OptionalProperty->GetValueProperty(), StorageError);
      if (!InnerDefault.IsValid()) {
        SetFailure(OutFailure, StorageError, StorageError, PropertyPath);
        return false;
      }
      InnerBase = InnerDefault->GetValuePtr();
    }
    TSharedPtr<FJsonValue> CompletedInner;
    FResolvedNode Child;
    if (!BuildCompletedValue(OptionalProperty->GetValueProperty(), InnerInput,
                             InnerBase, Owner, Prepared, Entry, Options, false,
                             false, Session, PropertyPath + TEXT(".value"),
                             CompletedInner, Child, OutFailure))
      return false;
    TSharedRef<FJsonObject> CompletedObject = MakeShared<FJsonObject>();
    CompletedObject->SetBoolField(TEXT("is_set"), true);
    CompletedObject->SetField(TEXT("value"), CompletedInner);
    OutNode.Children.Add(MoveTemp(Child));
    OutCompletedValue = MakeShared<FJsonValueObject>(CompletedObject);
    return true;
  }

  OutCompletedValue = InputValue;
  return true;
}

bool ReplaceScratchReferences(FProperty *Property, void *ValuePtr,
                              const TArray<FMaterialization> &Materializations,
                              bool bIgnoreFixedArray, FString &OutError) {
  if (!Property || !ValuePtr)
    return false;
  if (!bIgnoreFixedArray && Property->ArrayDim > 1) {
    for (int32 Index = 0; Index < Property->ArrayDim; ++Index) {
      if (!ReplaceScratchReferences(Property,
                                    static_cast<uint8 *>(ValuePtr) +
                                        Index * Property->GetElementSize(),
                                    Materializations, true, OutError))
        return false;
    }
    return true;
  }
  if (FObjectPropertyBase *ObjectProperty =
          CastField<FObjectPropertyBase>(Property)) {
    UObject *CurrentObject = ObjectProperty->GetObjectPropertyValue(ValuePtr);
    for (const FMaterialization &Materialization : Materializations) {
      if (CurrentObject == Materialization.ScratchObject.Get()) {
        ObjectProperty->SetObjectPropertyValue(
            ValuePtr, Materialization.CreatedObject.Get());
        break;
      }
    }
    return true;
  }
  if (FStructProperty *StructProperty = CastField<FStructProperty>(Property)) {
    for (TFieldIterator<FProperty> It(StructProperty->Struct,
                                      EFieldIterationFlags::IncludeSuper);
         It; ++It) {
      FProperty *Field = *It;
      if (!ReplaceScratchReferences(
              Field, Field->ContainerPtrToValuePtr<void>(ValuePtr),
              Materializations, false, OutError))
        return false;
    }
    return true;
  }
  if (FArrayProperty *ArrayProperty = CastField<FArrayProperty>(Property)) {
    FScriptArrayHelper Helper(ArrayProperty, ValuePtr);
    for (int32 Index = 0; Index < Helper.Num(); ++Index) {
      if (!ReplaceScratchReferences(ArrayProperty->Inner,
                                    Helper.GetRawPtr(Index), Materializations,
                                    false, OutError))
        return false;
    }
    return true;
  }
  if (FSetProperty *SetProperty = CastField<FSetProperty>(Property)) {
    FScriptSetHelper Helper(SetProperty, ValuePtr);
    for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index) {
      if (Helper.IsValidIndex(Index) &&
          !ReplaceScratchReferences(SetProperty->ElementProp,
                                    Helper.GetElementPtr(Index),
                                    Materializations, false, OutError))
        return false;
    }
    return true;
  }
  if (FMapProperty *MapProperty = CastField<FMapProperty>(Property)) {
    FScriptMapHelper Helper(MapProperty, ValuePtr);
    for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index) {
      if (Helper.IsValidIndex(Index) &&
          !ReplaceScratchReferences(MapProperty->ValueProp,
                                    Helper.GetValuePtr(Index), Materializations,
                                    false, OutError))
        return false;
    }
    return true;
  }
  if (FOptionalProperty *OptionalProperty =
          CastField<FOptionalProperty>(Property)) {
    return !OptionalProperty->IsSet(ValuePtr) ||
           ReplaceScratchReferences(
               OptionalProperty->GetValueProperty(),
               OptionalProperty->GetValuePointerForReadOrReplace(ValuePtr),
               Materializations, false, OutError);
  }
  return true;
}

bool ValidateOwnedReferences(FProperty *Property, const void *ValuePtr,
                             UObject *Owner,
                             const TArray<FMaterialization> &Materializations,
                             bool bIgnoreFixedArray, FString &OutError) {
  if (!Property || !ValuePtr)
    return false;
  if (!bIgnoreFixedArray && Property->ArrayDim > 1) {
    for (int32 Index = 0; Index < Property->ArrayDim; ++Index) {
      if (!ValidateOwnedReferences(Property,
                                   static_cast<const uint8 *>(ValuePtr) +
                                       Index * Property->GetElementSize(),
                                   Owner, Materializations, true, OutError))
        return false;
    }
    return true;
  }
  if (const FObjectPropertyBase *ObjectProperty =
          CastField<FObjectPropertyBase>(Property)) {
    if (!UeAgentAuthoringReflection::IsOwnedObjectProperty(Property))
      return true;
    UObject *Object = ObjectProperty->GetObjectPropertyValue(ValuePtr);
    if (!Object || (Owner && IsObjectWithin(Object, Owner)))
      return true;
    for (const FMaterialization &Materialization : Materializations) {
      if (Object == Materialization.ScratchObject.Get())
        return true;
    }
    OutError =
        TEXT("owned_object_reference_outside_owner:") + Object->GetPathName();
    return false;
  }
  if (const FStructProperty *StructProperty =
          CastField<FStructProperty>(Property)) {
    for (TFieldIterator<FProperty> It(StructProperty->Struct,
                                      EFieldIterationFlags::IncludeSuper);
         It; ++It) {
      FProperty *Field = *It;
      if (!ValidateOwnedReferences(
              Field, Field->ContainerPtrToValuePtr<void>(ValuePtr), Owner,
              Materializations, false, OutError))
        return false;
    }
    return true;
  }
  if (const FArrayProperty *ArrayProperty =
          CastField<FArrayProperty>(Property)) {
    FScriptArrayHelper Helper(ArrayProperty, ValuePtr);
    for (int32 Index = 0; Index < Helper.Num(); ++Index) {
      if (!ValidateOwnedReferences(ArrayProperty->Inner,
                                   Helper.GetRawPtr(Index), Owner,
                                   Materializations, false, OutError))
        return false;
    }
    return true;
  }
  if (const FSetProperty *SetProperty = CastField<FSetProperty>(Property)) {
    FScriptSetHelper Helper(SetProperty, ValuePtr);
    for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index) {
      if (Helper.IsValidIndex(Index) &&
          !ValidateOwnedReferences(SetProperty->ElementProp,
                                   Helper.GetElementPtr(Index), Owner,
                                   Materializations, false, OutError))
        return false;
    }
    return true;
  }
  if (const FMapProperty *MapProperty = CastField<FMapProperty>(Property)) {
    FScriptMapHelper Helper(MapProperty, ValuePtr);
    for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index) {
      if (!Helper.IsValidIndex(Index))
        continue;
      if (!ValidateOwnedReferences(MapProperty->KeyProp,
                                   Helper.GetKeyPtr(Index), Owner,
                                   Materializations, false, OutError) ||
          !ValidateOwnedReferences(MapProperty->ValueProp,
                                   Helper.GetValuePtr(Index), Owner,
                                   Materializations, false, OutError))
        return false;
    }
    return true;
  }
  if (const FOptionalProperty *OptionalProperty =
          CastField<FOptionalProperty>(Property)) {
    return !OptionalProperty->IsSet(ValuePtr) ||
           ValidateOwnedReferences(
               OptionalProperty->GetValueProperty(),
               OptionalProperty->GetValuePointerForRead(ValuePtr), Owner,
               Materializations, false, OutError);
  }
  return true;
}

bool ValidateEditConditions(FPreparedApply::FImpl &Prepared,
                            FFailure &OutFailure) {
  bool bHasConditions = false;
  for (const FPreparedProperty &Entry : Prepared.Properties) {
    bHasConditions |=
        Entry.Property && Entry.Property->HasMetaData(TEXT("EditCondition"));
  }
  if (!bHasConditions)
    return true;

  TArray<UeAgentAuthoringTargetLocator::FResolvedPropertyTarget> ScratchTargets;
  TSharedPtr<FStructOnScope> ScratchStruct;
  UObject *ScratchRoot = nullptr;
  UeAgentAuthoringNativeDefaults::FScratchScope EditConditionScratch;
  if (!Prepared.RootObject) {
    ScratchStruct = MakeShared<FStructOnScope>(Prepared.RootStruct);
    if (!ScratchStruct->IsValid()) {
      SetFailure(OutFailure, TEXT("scratch_struct_creation_failed"),
                 Prepared.RootOwnerPath);
      return false;
    }
    if (const UScriptStruct *ScriptStruct =
            Cast<UScriptStruct>(Prepared.RootStruct)) {
      ScriptStruct->CopyScriptStruct(ScratchStruct->GetStructMemory(),
                                     Prepared.RootData);
    } else {
      for (TFieldIterator<FProperty> It(Prepared.RootStruct); It; ++It) {
        It->CopyCompleteValue_InContainer(ScratchStruct->GetStructMemory(),
                                          Prepared.RootData);
      }
    }
    for (const FPreparedProperty &Entry : Prepared.Properties) {
      UeAgentAuthoringTargetLocator::FResolvedPropertyTarget ScratchTarget;
      UeAgentAuthoringTargetLocator::FResolveResult ResolveResult;
      if (!UeAgentAuthoringTargetLocator::ResolvePropertyPath(
              Prepared.RootStruct, ScratchStruct->GetStructMemory(),
              Prepared.RootOwnerPath, Prepared.NotificationOwner,
              Entry.PropertyPath, ScratchTarget, ResolveResult) ||
          !ScratchTarget.Property || !ScratchTarget.ValuePtr) {
        SetFailure(OutFailure, TEXT("scratch_property_resolution_failed"),
                   Entry.PropertyPath, Entry.PropertyPath);
        return false;
      }
      ScratchTarget.Property->CopyCompleteValue(
          ScratchTarget.ValuePtr, Entry.DesiredValue->GetValuePtr());
      ScratchTargets.Add(MoveTemp(ScratchTarget));
    }
  } else {
    FString ScratchError;
    UPackage *ScratchPackage =
        EditConditionScratch.CreateScratchPackage(ScratchError);
    const bool bRequiresAssetContext = Prepared.RootObject->IsAsset();
    if (ScratchPackage && bRequiresAssetContext) {
      ScratchPackage->ClearFlags(RF_Transient);
    }
    TMap<UObject *, UObject *> CreatedObjects;
    if (ScratchPackage) {
      FObjectDuplicationParameters DuplicationParameters(Prepared.RootObject,
                                                         ScratchPackage);
      DuplicationParameters.DestName = Prepared.RootObject->GetFName();
      DuplicationParameters.FlagMask &= ~RF_Standalone;
      DuplicationParameters.bAssignExternalPackages = false;
      DuplicationParameters.CreatedObjects = &CreatedObjects;
      ScratchRoot = StaticDuplicateObjectEx(DuplicationParameters);
    }
    if (!ScratchRoot) {
      SetFailure(OutFailure,
                 ScratchError.IsEmpty()
                     ? TEXT("scratch_object_duplication_failed")
                     : ScratchError,
                 Prepared.RootOwnerPath);
      return false;
    }
    if (!EditConditionScratch.AdoptObjectFromScratchPackage(ScratchRoot,
                                                            ScratchError)) {
      SetFailure(OutFailure, ScratchError, Prepared.RootOwnerPath);
      return false;
    }
    for (const TPair<UObject *, UObject *> &Pair : CreatedObjects) {
      if (Pair.Value && !EditConditionScratch.AdoptObjectFromScratchPackage(
                            Pair.Value, ScratchError)) {
        SetFailure(OutFailure, ScratchError, Prepared.RootOwnerPath);
        return false;
      }
    }
    if (ScratchRoot->IsAsset() != bRequiresAssetContext) {
      SetFailure(OutFailure, TEXT("scratch_object_asset_context_mismatch"),
                 Prepared.RootOwnerPath);
      return false;
    }
    for (const FPreparedProperty &Entry : Prepared.Properties) {
      UeAgentAuthoringTargetLocator::FResolvedPropertyTarget ScratchTarget;
      UeAgentAuthoringTargetLocator::FResolveResult ResolveResult;
      if (!UeAgentAuthoringTargetLocator::ResolvePropertyPath(
              ScratchRoot, Entry.PropertyPath, ScratchTarget, ResolveResult) ||
          !ScratchTarget.Property || !ScratchTarget.ValuePtr) {
        SetFailure(OutFailure, TEXT("scratch_property_resolution_failed"),
                   Entry.PropertyPath, Entry.PropertyPath);
        return false;
      }
      ScratchTarget.Property->CopyCompleteValue(
          ScratchTarget.ValuePtr, Entry.DesiredValue->GetValuePtr());
      ScratchTargets.Add(MoveTemp(ScratchTarget));
    }
  }

  FPropertyEditorModule *PropertyEditor =
      FModuleManager::LoadModulePtr<FPropertyEditorModule>(
          TEXT("PropertyEditor"));
  if (!PropertyEditor) {
    SetFailure(OutFailure,
               TEXT("property_edit_condition_evaluator_unavailable"),
               Prepared.RootOwnerPath);
    return false;
  }
  FPropertyRowGeneratorArgs Args;
  Args.bShouldShowHiddenProperties = true;
  const TSharedPtr<IPropertyRowGenerator> ScratchRows =
      PropertyEditor->CreatePropertyRowGenerator(Args);
  if (ScratchStruct.IsValid())
    ScratchRows->SetStructure(ScratchStruct);
  else
    ScratchRows->SetObjects({ScratchRoot});

  auto FindScratchHandle = [&ScratchRows](const FProperty *Property,
                                          const void *ValuePtr) {
    TFunction<TSharedPtr<IPropertyHandle>(
        const TArray<TSharedRef<IDetailTreeNode>> &)>
        FindInNodes;
    FindInNodes = [&FindInNodes, Property,
                   ValuePtr](const TArray<TSharedRef<IDetailTreeNode>> &Nodes) {
      for (const TSharedRef<IDetailTreeNode> &Node : Nodes) {
        const TSharedPtr<IPropertyHandle> Handle = Node->CreatePropertyHandle();
        void *HandleValuePtr = nullptr;
        if (Handle.IsValid() && Handle->IsValidHandle() &&
            Handle->GetProperty() == Property &&
            Handle->GetValueData(HandleValuePtr) == FPropertyAccess::Success &&
            HandleValuePtr == ValuePtr) {
          return Handle;
        }
        TArray<TSharedRef<IDetailTreeNode>> Children;
        Node->GetChildren(Children, true);
        if (const TSharedPtr<IPropertyHandle> Child = FindInNodes(Children);
            Child.IsValid())
          return Child;
      }
      return TSharedPtr<IPropertyHandle>();
    };
    return ScratchRows.IsValid() ? FindInNodes(ScratchRows->GetRootTreeNodes())
                                 : TSharedPtr<IPropertyHandle>();
  };

  for (int32 Index = 0; Index < Prepared.Properties.Num(); ++Index) {
    const FPreparedProperty &Entry = Prepared.Properties[Index];
    if (!Entry.Property->HasMetaData(TEXT("EditCondition")))
      continue;
    const TSharedPtr<IPropertyHandle> Handle = FindScratchHandle(
        ScratchTargets[Index].Property, ScratchTargets[Index].ValuePtr);
    if (!Handle.IsValid()) {
      SetFailure(OutFailure, TEXT("property_edit_condition_evaluation_failed"),
                 Entry.PropertyPath, Entry.PropertyPath);
      return false;
    }
    if (!Handle->IsEditable()) {
      SetFailure(OutFailure, TEXT("property_edit_condition_false"),
                 Entry.PropertyPath, Entry.PropertyPath);
      return false;
    }
  }
  return true;
}

bool ValidateDescriptor(const TSharedPtr<FJsonObject> &Descriptor,
                        FString &OutPropertyPath,
                        TSharedPtr<FJsonValue> &OutValue,
                        FFailure &OutFailure) {
  if (!Descriptor.IsValid()) {
    SetFailure(OutFailure, TEXT("invalid_property_descriptor"),
               TEXT("object_required"));
    return false;
  }
  for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair :
       Descriptor->Values) {
    if (!PropertyDescriptorFields().Contains(Pair.Key)) {
      SetFailure(OutFailure, TEXT("unexpected_property_descriptor_field"),
                 Pair.Key);
      return false;
    }
  }
  if (!Descriptor->TryGetStringField(TEXT("property_path"), OutPropertyPath) ||
      OutPropertyPath.IsEmpty()) {
    SetFailure(OutFailure, TEXT("missing_property_path"),
               TEXT("property_path"));
    return false;
  }
  OutValue = Descriptor->TryGetField(TEXT("value"));
  if (!OutValue.IsValid()) {
    SetFailure(OutFailure, TEXT("missing_value"), OutPropertyPath,
               OutPropertyPath);
    return false;
  }
  return true;
}

bool ValidateOptionalTypeAssertions(const FProperty *Property,
                                    const TSharedPtr<FJsonObject> &Descriptor,
                                    FString &OutError) {
  TSharedPtr<FJsonObject> Completed =
      UeAgentPropertyValueCodec::BuildTypeDescriptor(Property);
  if (!Completed.IsValid()) {
    OutError = TEXT("invalid_type_descriptor");
    return false;
  }
  FString Kind;
  if (!Completed->TryGetStringField(TEXT("kind"), Kind)) {
    OutError = TEXT("missing_value_kind");
    return false;
  }
  Completed->RemoveField(TEXT("kind"));
  Completed->SetStringField(TEXT("value_kind"), Kind);
  static const TSet<FString> AssertionFields = {
      TEXT("value_kind"),   TEXT("struct_path"),  TEXT("enum_path"),
      TEXT("element_type"), TEXT("key_type"),     TEXT("mapped_type"),
      TEXT("inner_type"),   TEXT("fixed_length"), TEXT("allowed_class")};
  for (const FString &Field : AssertionFields) {
    if (const TSharedPtr<FJsonValue> Assertion = Descriptor->TryGetField(Field);
        Assertion.IsValid()) {
      Completed->SetField(Field, Assertion);
    }
  }
  return UeAgentPropertyValueCodec::ValidateTypeMetadata(Property, Completed,
                                                         OutError);
}

bool PrepareRootInternal(
    UStruct *RootStruct, void *RootData, UObject *RootObject,
    UObject *NotificationOwner, const FString &RootOwnerPath,
    const TArray<TSharedPtr<FJsonValue>> &PropertyDescriptors,
    const FOptions &Options, FPrepareSession &Session,
    FPreparedApply &OutPrepared, FFailure &OutFailure) {
  OutPrepared = FPreparedApply();
  OutFailure = FFailure();
  if (!RootStruct || !RootData || RootOwnerPath.IsEmpty()) {
    SetFailure(OutFailure, TEXT("object_apply_root_invalid"), RootOwnerPath);
    return false;
  }
  if (!ValidateContainerCount(Options, PropertyDescriptors.Num(), OutFailure,
                              TEXT("properties")))
    return false;
  if (RootObject) {
    FString OwnerReason;
    if (!ValidateRootOwner(RootObject, Options, OwnerReason)) {
      SetFailure(OutFailure, OwnerReason, RootObject->GetPathName());
      return false;
    }
    if (Session.ObjectStack.Contains(RootObject)) {
      SetFailure(OutFailure, TEXT("object_identity_cycle"),
                 RootObject->GetPathName());
      return false;
    }
    Session.ObjectStack.Add(RootObject);
  }

  FPreparedApply::FImpl &Prepared = FPreparedApplyAccess::Get(OutPrepared);
  Prepared.RootStruct = RootStruct;
  Prepared.RootData = RootData;
  Prepared.RootObject = RootObject;
  Prepared.NotificationOwner = NotificationOwner;
  Prepared.RootOwnerPath = RootOwnerPath;
  Prepared.Options = Options;

  TSet<FString> SeenPaths;
  for (const TSharedPtr<FJsonValue> &DescriptorValue : PropertyDescriptors) {
    const TSharedPtr<FJsonObject> Descriptor =
        DescriptorValue.IsValid() && DescriptorValue->Type == EJson::Object
            ? DescriptorValue->AsObject()
            : nullptr;
    FString PropertyPath;
    TSharedPtr<FJsonValue> InputValue;
    if (!ValidateDescriptor(Descriptor, PropertyPath, InputValue, OutFailure)) {
      if (RootObject)
        Session.ObjectStack.Remove(RootObject);
      return false;
    }
    if (SeenPaths.Contains(PropertyPath)) {
      SetFailure(OutFailure, TEXT("duplicate_property_path"), PropertyPath,
                 PropertyPath);
      if (RootObject)
        Session.ObjectStack.Remove(RootObject);
      return false;
    }
    for (const FString &ExistingPath : SeenPaths) {
      if (PropertyPath.StartsWith(ExistingPath + TEXT(".")) ||
          ExistingPath.StartsWith(PropertyPath + TEXT("."))) {
        SetFailure(OutFailure, TEXT("overlapping_property_path"),
                   PropertyPath + TEXT(":") + ExistingPath, PropertyPath);
        if (RootObject)
          Session.ObjectStack.Remove(RootObject);
        return false;
      }
    }
    SeenPaths.Add(PropertyPath);

    UeAgentAuthoringTargetLocator::FResolvedPropertyTarget Target;
    UeAgentAuthoringTargetLocator::FResolveResult TargetResult;
    if (!UeAgentAuthoringTargetLocator::ResolvePropertyPath(
            RootStruct, RootData, RootOwnerPath, NotificationOwner,
            PropertyPath, Target, TargetResult) ||
        !Target.Property || !Target.ValuePtr) {
      SetFailure(OutFailure, TEXT("property_not_found"), TargetResult.ErrorCode,
                 PropertyPath);
      if (RootObject)
        Session.ObjectStack.Remove(RootObject);
      return false;
    }
    if (Target.NotificationOwner && RootObject &&
        !IsObjectWithin(Target.NotificationOwner, RootObject)) {
      SetFailure(OutFailure,
                 TEXT("external_reference_recursive_patch_forbidden"),
                 Target.NotificationOwner->GetPathName(), PropertyPath);
      Session.ObjectStack.Remove(RootObject);
      return false;
    }
    if (Target.NotificationOwner && !RootObject &&
        Target.NotificationOwner != NotificationOwner) {
      SetFailure(OutFailure,
                 TEXT("external_reference_recursive_patch_forbidden"),
                 PropertyPath, PropertyPath);
      return false;
    }

    FString PropertyReason;
    if (!IsEditableForApply(Target.Property, Target.NotificationOwner, Options,
                            PropertyReason)) {
      SetFailure(OutFailure, PropertyReason, Target.Property->GetName(),
                 PropertyPath);
      if (RootObject)
        Session.ObjectStack.Remove(RootObject);
      return false;
    }
    FString CodecReason;
    if (!ValidateOptionalTypeAssertions(Target.Property, Descriptor,
                                        CodecReason)) {
      SetFailure(OutFailure, CodecReason, CodecReason, PropertyPath);
      if (RootObject)
        Session.ObjectStack.Remove(RootObject);
      return false;
    }

    FString StorageError;
    FPreparedProperty Entry;
    Entry.PropertyPath = PropertyPath;
    Entry.NotificationOwner = Target.NotificationOwner;
    Entry.Property = Target.Property;
    Entry.CurrentValuePtr = Target.ValuePtr;
    Entry.DesiredValue = FPropertyValueStorage::CopyFrom(
        Target.Property, Target.ValuePtr, StorageError);
    Entry.OriginalValue = FPropertyValueStorage::CopyFrom(
        Target.Property, Target.ValuePtr, StorageError);
    if (!Entry.DesiredValue.IsValid() || !Entry.OriginalValue.IsValid()) {
      SetFailure(OutFailure, StorageError, StorageError, PropertyPath);
      if (RootObject)
        Session.ObjectStack.Remove(RootObject);
      return false;
    }

    TSharedPtr<FJsonValue> CompletedValue;
    if (!BuildCompletedValue(Target.Property, InputValue, Target.ValuePtr,
                             Target.NotificationOwner, Prepared, Entry, Options,
                             false, false, Session, PropertyPath,
                             CompletedValue, Entry.Node, OutFailure)) {
      if (RootObject)
        Session.ObjectStack.Remove(RootObject);
      return false;
    }
    UeAgentPropertyValueCodec::FOptions CodecOptions =
        MakeCodecOptions(Options);
    if (!UeAgentPropertyValueCodec::Decode(
            Target.Property, CompletedValue,
            Entry.DesiredValue->GetMutableValuePtr(), Target.NotificationOwner,
            CodecOptions, CodecReason)) {
      SetFailure(OutFailure, CodecReason, CodecReason, PropertyPath);
      if (RootObject)
        Session.ObjectStack.Remove(RootObject);
      return false;
    }
    FString OwnedReferenceError;
    if (!ValidateOwnedReferences(
            Target.Property, Entry.DesiredValue->GetValuePtr(),
            Target.NotificationOwner, Entry.Materializations, false,
            OwnedReferenceError)) {
      SetFailure(OutFailure, TEXT("owned_object_reference_outside_owner"),
                 OwnedReferenceError, PropertyPath);
      if (RootObject)
        Session.ObjectStack.Remove(RootObject);
      return false;
    }
    if (Entry.NestedOwnedPlans.IsEmpty() && Entry.Materializations.IsEmpty()) {
      bool bEquivalent = false;
      FString CompareError;
      if (!UeAgentAuthoringSemanticCompare::ArePropertyValuesEquivalent(
              Target.Property, Target.ValuePtr,
              Entry.DesiredValue->GetValuePtr(), Target.NotificationOwner,
              bEquivalent, CompareError)) {
        SetFailure(OutFailure,
                   CompareError.IsEmpty()
                       ? TEXT("property_semantic_compare_failed")
                       : CompareError,
                   PropertyPath, PropertyPath);
        if (RootObject)
          Session.ObjectStack.Remove(RootObject);
        return false;
      }
      if (bEquivalent)
        continue;
    }
    Prepared.Nodes.Add(Entry.Node);
    Prepared.Properties.Add(MoveTemp(Entry));
  }

  if (!ValidateEditConditions(Prepared, OutFailure)) {
    if (RootObject)
      Session.ObjectStack.Remove(RootObject);
    return false;
  }
  Prepared.bReady = true;
  if (RootObject)
    Session.ObjectStack.Remove(RootObject);
  return true;
}

void RecordPackageState(FPreparedApply::FImpl &Prepared, UObject *Owner) {
  if (!Owner || !Owner->GetPackage())
    return;
  Prepared.PackageDirtyStates.FindOrAdd(Owner->GetPackage(),
                                        Owner->GetPackage()->IsDirty());
}

bool MaterializeEntry(FPreparedApply::FImpl &Prepared, FPreparedProperty &Entry,
                      FApplyResult &OutResult) {
  if (Entry.Materializations.IsEmpty())
    return true;
  UObject *Outer = Entry.NotificationOwner ? Entry.NotificationOwner
                                           : Prepared.NotificationOwner;
  if (!Outer) {
    OutResult.ErrorCode = TEXT("owned_object_real_outer_missing");
    OutResult.ErrorDetail = Entry.PropertyPath;
    OutResult.PropertyPath = Entry.PropertyPath;
    return false;
  }
  for (FMaterialization &Materialization : Entry.Materializations) {
    UObject *ScratchObject = Materialization.ScratchObject.Get();
    if (!ScratchObject) {
      OutResult.ErrorCode = TEXT("owned_object_scratch_expired");
      OutResult.ErrorDetail = Entry.PropertyPath;
      OutResult.PropertyPath = Entry.PropertyPath;
      return false;
    }
    UObject *Created = StaticDuplicateObject(ScratchObject, Outer, NAME_None,
                                             RF_Transactional);
    if (!Created) {
      OutResult.ErrorCode = TEXT("owned_object_create_failed");
      OutResult.ErrorDetail = Entry.PropertyPath;
      OutResult.PropertyPath = Entry.PropertyPath;
      return false;
    }
    Materialization.CreatedObject.Reset(Created);
  }
  FString ReplaceError;
  if (!ReplaceScratchReferences(Entry.Property,
                                Entry.DesiredValue->GetMutableValuePtr(),
                                Entry.Materializations, false, ReplaceError)) {
    OutResult.ErrorCode = TEXT("owned_object_materialization_replace_failed");
    OutResult.ErrorDetail = ReplaceError;
    OutResult.PropertyPath = Entry.PropertyPath;
    return false;
  }
  return true;
}

bool ApplyPreparedInternal(FPreparedApply::FImpl &Prepared,
                           FApplyResult &OutResult) {
  OutResult = FApplyResult();
  OutResult.Status = TEXT("failed");
  OutResult.PropertiesPrepared = Prepared.Properties.Num();
  if (!Prepared.bReady || Prepared.bApplied || Prepared.bRolledBack) {
    OutResult.ErrorCode = TEXT("prepared_apply_state_invalid");
    OutResult.ErrorDetail = Prepared.RootOwnerPath;
    return false;
  }

  for (int32 Index = 0; Index < Prepared.Properties.Num(); ++Index) {
    FPreparedProperty &Entry = Prepared.Properties[Index];
    Prepared.PropertiesAttempted = Index + 1;
    OutResult.PropertiesAttempted = Prepared.PropertiesAttempted;
    if (!MaterializeEntry(Prepared, Entry, OutResult))
      return false;

    UObject *Owner = Entry.NotificationOwner;
    RecordPackageState(Prepared, Owner);
    if (Owner)
      Owner->Modify();
    const UClass *PropertyOwnerClass =
        Cast<UClass>(Entry.Property->GetOwnerStruct());
    const bool bCanNotify = Prepared.Options.bEmitEditorNotifications &&
                            Owner && PropertyOwnerClass &&
                            Owner->IsA(PropertyOwnerClass);
    if (bCanNotify)
      Owner->PreEditChange(Entry.Property);
    Entry.Property->CopyCompleteValue(Entry.CurrentValuePtr,
                                      Entry.DesiredValue->GetValuePtr());
    Entry.bPropertyApplied = true;
    if (bCanNotify) {
      FPropertyChangedEvent ChangeEvent(Entry.Property,
                                        EPropertyChangeType::ValueSet);
      Owner->PostEditChangeProperty(ChangeEvent);
    }

    for (int32 NestedIndex = 0; NestedIndex < Entry.NestedOwnedPlans.Num();
         ++NestedIndex) {
      Entry.NestedPlansAttempted = NestedIndex + 1;
      FApplyResult NestedResult;
      if (!ApplyPrepared(*Entry.NestedOwnedPlans[NestedIndex], NestedResult)) {
        OutResult.ErrorCode = NestedResult.ErrorCode;
        OutResult.ErrorDetail = NestedResult.ErrorDetail;
        OutResult.PropertyPath =
            Entry.PropertyPath + TEXT(".") + NestedResult.PropertyPath;
        return false;
      }
    }
    if (Prepared.Options.bMarkPackageDirty && Owner)
      Owner->MarkPackageDirty();
    OutResult.PropertiesApplied = Index + 1;
  }
  Prepared.bApplied = true;
  OutResult.Status = TEXT("applied");
  return true;
}

bool VerifyPreparedInternal(FPreparedApply::FImpl &Prepared,
                            FApplyResult &OutResult) {
  if (!Prepared.bApplied || Prepared.bRolledBack) {
    OutResult.Status = TEXT("failed");
    OutResult.ErrorCode = TEXT("prepared_apply_not_applied");
    OutResult.ErrorDetail = Prepared.RootOwnerPath;
    return false;
  }
  for (FPreparedProperty &Entry : Prepared.Properties) {
    bool bEquivalent = false;
    FString CompareError;
    if (!UeAgentAuthoringSemanticCompare::ArePropertyValuesEquivalent(
            Entry.Property, Entry.CurrentValuePtr,
            Entry.DesiredValue->GetValuePtr(), Entry.NotificationOwner,
            bEquivalent, CompareError) ||
        !bEquivalent) {
      OutResult.Status = TEXT("failed");
      OutResult.ErrorCode =
          CompareError.IsEmpty() ? TEXT("readback_mismatch") : CompareError;
      OutResult.ErrorDetail = Entry.PropertyPath;
      OutResult.PropertyPath = Entry.PropertyPath;
      return false;
    }
    for (const TUniquePtr<FPreparedApply> &NestedPlan :
         Entry.NestedOwnedPlans) {
      FApplyResult NestedResult;
      if (!VerifyPrepared(*NestedPlan, NestedResult)) {
        OutResult.Status = TEXT("failed");
        OutResult.ErrorCode = NestedResult.ErrorCode;
        OutResult.ErrorDetail = NestedResult.ErrorDetail;
        OutResult.PropertyPath =
            Entry.PropertyPath + TEXT(".") + NestedResult.PropertyPath;
        return false;
      }
    }
  }
  OutResult.Status = TEXT("applied");
  OutResult.ErrorCode.Reset();
  OutResult.ErrorDetail.Reset();
  OutResult.PropertyPath.Reset();
  OutResult.PropertiesPrepared = Prepared.Properties.Num();
  OutResult.PropertiesAttempted = Prepared.Properties.Num();
  OutResult.PropertiesApplied = Prepared.Properties.Num();
  return true;
}

void CleanupCreatedObjects(FPreparedProperty &Entry) {
  for (int32 Index = Entry.Materializations.Num() - 1; Index >= 0; --Index) {
    FMaterialization &Materialization = Entry.Materializations[Index];
    UObject *Created = Materialization.CreatedObject.Get();
    if (!Created)
      continue;
    Created->ClearFlags(RF_Public | RF_Standalone | RF_Transactional);
    Created->SetFlags(RF_Transient);
    Created->MarkAsGarbage();
    Materialization.CreatedObject.Reset();
  }
}

bool RollbackPreparedInternal(FPreparedApply::FImpl &Prepared,
                              FApplyResult &OutResult) {
  OutResult.bRollbackAttempted = true;
  bool bSucceeded = true;
  for (int32 Index = Prepared.PropertiesAttempted - 1; Index >= 0; --Index) {
    FPreparedProperty &Entry = Prepared.Properties[Index];
    for (int32 NestedIndex = Entry.NestedPlansAttempted - 1; NestedIndex >= 0;
         --NestedIndex) {
      FApplyResult NestedResult;
      if (!RollbackPrepared(*Entry.NestedOwnedPlans[NestedIndex], NestedResult))
        bSucceeded = false;
    }
    if (Entry.bPropertyApplied) {
      UObject *Owner = Entry.NotificationOwner;
      const UClass *PropertyOwnerClass =
          Cast<UClass>(Entry.Property->GetOwnerStruct());
      const bool bCanNotify = Prepared.Options.bEmitEditorNotifications &&
                              Owner && PropertyOwnerClass &&
                              Owner->IsA(PropertyOwnerClass);
      if (bCanNotify)
        Owner->PreEditChange(Entry.Property);
      Entry.Property->CopyCompleteValue(Entry.CurrentValuePtr,
                                        Entry.OriginalValue->GetValuePtr());
      if (bCanNotify) {
        FPropertyChangedEvent ChangeEvent(Entry.Property,
                                          EPropertyChangeType::ValueSet);
        Owner->PostEditChangeProperty(ChangeEvent);
      }
      ++OutResult.PropertiesRolledBack;
    }
    CleanupCreatedObjects(Entry);
  }
  for (const TPair<UPackage *, bool> &Pair : Prepared.PackageDirtyStates) {
    if (Pair.Key)
      Pair.Key->SetDirtyFlag(Pair.Value);
  }
  Prepared.bRolledBack = true;
  Prepared.bApplied = false;
  OutResult.Status = bSucceeded ? TEXT("rolled_back") : TEXT("rollback_failed");
  OutResult.bRollbackSucceeded = bSucceeded;
  if (!bSucceeded && OutResult.ErrorCode.IsEmpty())
    OutResult.ErrorCode = TEXT("object_apply_rollback_failed");
  return bSucceeded;
}
} // namespace

namespace UeAgentAuthoringObjectApply {
FDomainOwnerAuthorizationHandle
RegisterDomainAuthorizedOwnerPackagePrefix(const FString &PackagePrefix) {
  FString NormalizedPrefix = PackagePrefix;
  NormalizedPrefix.TrimStartAndEndInline();
  if (!NormalizedPrefix.StartsWith(TEXT("/")) || NormalizedPrefix.Len() <= 1 ||
      NormalizedPrefix.StartsWith(TEXT("/Engine"), ESearchCase::IgnoreCase)) {
    return 0;
  }

  FScopeLock Lock(&GDomainOwnerAuthorizationMutex);
  const FDomainOwnerAuthorizationHandle Handle =
      GNextDomainOwnerAuthorizationHandle++;
  GDomainOwnerAuthorizationPrefixes.Add(Handle, MoveTemp(NormalizedPrefix));
  return Handle;
}

void UnregisterDomainAuthorizedOwnerPackagePrefix(
    FDomainOwnerAuthorizationHandle Handle) {
  if (Handle == 0)
    return;
  FScopeLock Lock(&GDomainOwnerAuthorizationMutex);
  GDomainOwnerAuthorizationPrefixes.Remove(Handle);
}

TSharedPtr<FJsonObject>
ProjectDescriptorForApply(const TSharedPtr<FJsonObject> &ExportDescriptor) {
  if (!ExportDescriptor.IsValid() ||
      !ExportDescriptor->HasTypedField<EJson::String>(TEXT("property_path")) ||
      !ExportDescriptor->HasField(TEXT("value"))) {
    return nullptr;
  }
  const TSharedPtr<FJsonValue> Projected =
      ProjectValueForApply(MakeShared<FJsonValueObject>(ExportDescriptor));
  return Projected.IsValid() && Projected->Type == EJson::Object
             ? Projected->AsObject()
             : nullptr;
}

TSharedRef<FJsonObject> FResolvedNode::ToJsonObject() const {
  TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
  Object->SetStringField(TEXT("property_path"), PropertyPath);
  Object->SetStringField(TEXT("value_kind"), ValueKind);
  if (!TypePath.IsEmpty())
    Object->SetStringField(TEXT("type_path"), TypePath);
  Object->SetBoolField(TEXT("explicit"), bExplicit);
  TArray<TSharedPtr<FJsonValue>> ChildValues;
  for (const FResolvedNode &Child : Children) {
    ChildValues.Add(MakeShared<FJsonValueObject>(Child.ToJsonObject()));
  }
  if (!ChildValues.IsEmpty())
    Object->SetArrayField(TEXT("children"), ChildValues);
  return Object;
}

bool FApplyResult::IsApplied() const {
  return Status == TEXT("applied") && ErrorCode.IsEmpty();
}

TSharedRef<FJsonObject> FApplyResult::ToJsonObject() const {
  TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
  Object->SetStringField(TEXT("schema"), TEXT("uai.object_apply_result.v1"));
  Object->SetStringField(TEXT("status"), Status);
  if (!ErrorCode.IsEmpty())
    Object->SetStringField(TEXT("error"), ErrorCode);
  if (!ErrorDetail.IsEmpty())
    Object->SetStringField(TEXT("error_detail"), ErrorDetail);
  if (!PropertyPath.IsEmpty())
    Object->SetStringField(TEXT("property_path"), PropertyPath);
  Object->SetNumberField(TEXT("properties_prepared"), PropertiesPrepared);
  Object->SetNumberField(TEXT("properties_attempted"), PropertiesAttempted);
  Object->SetNumberField(TEXT("properties_applied"), PropertiesApplied);
  Object->SetNumberField(TEXT("properties_rolled_back"), PropertiesRolledBack);
  Object->SetBoolField(TEXT("rollback_attempted"), bRollbackAttempted);
  Object->SetBoolField(TEXT("rollback_succeeded"), bRollbackSucceeded);
  return Object;
}

FPreparedApply::FPreparedApply() : Impl(MakeUnique<FImpl>()) {}

FPreparedApply::~FPreparedApply() = default;
FPreparedApply::FPreparedApply(FPreparedApply &&) noexcept = default;
FPreparedApply &FPreparedApply::operator=(FPreparedApply &&) noexcept = default;

bool FPreparedApply::IsReady() const {
  return Impl.IsValid() && Impl->bReady && !Impl->bApplied &&
         !Impl->bRolledBack;
}

bool FPreparedApply::HasBeenApplied() const {
  return Impl.IsValid() && Impl->bApplied && !Impl->bRolledBack;
}

int32 FPreparedApply::NumProperties() const {
  return Impl.IsValid() ? Impl->Properties.Num() : 0;
}

const TArray<FResolvedNode> &FPreparedApply::GetResolvedNodes() const {
  static const TArray<FResolvedNode> Empty;
  return Impl.IsValid() ? Impl->Nodes : Empty;
}

bool PrepareObject(UObject *RootObject,
                   const TArray<TSharedPtr<FJsonValue>> &PropertyDescriptors,
                   const FOptions &Options, FPreparedApply &OutPrepared,
                   FFailure &OutFailure) {
  FPrepareSession Session;
  return PrepareRootInternal(
      RootObject ? RootObject->GetClass() : nullptr, RootObject, RootObject,
      RootObject, RootObject ? RootObject->GetPathName() : FString(),
      PropertyDescriptors, Options, Session, OutPrepared, OutFailure);
}

bool PrepareStruct(UStruct *RootStruct, void *RootData,
                   UObject *NotificationOwner,
                   const TArray<TSharedPtr<FJsonValue>> &PropertyDescriptors,
                   const FOptions &Options, FPreparedApply &OutPrepared,
                   FFailure &OutFailure) {
  FPrepareSession Session;
  const FString RootOwnerPath =
      NotificationOwner ? NotificationOwner->GetPathName()
                        : (RootStruct ? RootStruct->GetPathName() : FString());
  return PrepareRootInternal(RootStruct, RootData, nullptr, NotificationOwner,
                             RootOwnerPath, PropertyDescriptors, Options,
                             Session, OutPrepared, OutFailure);
}

bool ApplyPrepared(FPreparedApply &Prepared, FApplyResult &OutResult) {
  return Prepared.Impl.IsValid() &&
         ApplyPreparedInternal(*Prepared.Impl, OutResult);
}

bool VerifyPrepared(FPreparedApply &Prepared, FApplyResult &OutResult) {
  return Prepared.Impl.IsValid() &&
         VerifyPreparedInternal(*Prepared.Impl, OutResult);
}

bool RollbackPrepared(FPreparedApply &Prepared, FApplyResult &OutResult) {
  if (!Prepared.Impl.IsValid()) {
    OutResult = FApplyResult();
    OutResult.Status = TEXT("rollback_failed");
    OutResult.ErrorCode = TEXT("prepared_apply_missing");
    return false;
  }
  return RollbackPreparedInternal(*Prepared.Impl, OutResult);
}
} // namespace UeAgentAuthoringObjectApply
