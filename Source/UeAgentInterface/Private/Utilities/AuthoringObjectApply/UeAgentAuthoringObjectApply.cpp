// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringObjectApply/UeAgentAuthoringObjectApply.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringJsonFields.h"
#include "Utilities/AuthoringObjectApply/UeAgentOwnedObjectCreation.h"
#include "Utilities/AuthoringObjectApply/UeAgentTemplatePropertyPropagation.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Core/HjsonWorkspace/UeAgentReflectedPropertyRows.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

#include "Utilities/AuthoringCore/UeAgentAuthoringNativeDefaults.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringPropertyValues.h"
#include "Utilities/AuthoringCore/UeAgentNativeNormalizationIntent.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringReflectionDescriptors.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticCompare.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTargetLocator.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTypeDescriptors.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Utilities/UeAgentEnvTraceDataValue.h"
#include "Utilities/UeAgentBlackboardKeySelectorValue.h"
#include "EnvironmentQuery/EnvQueryTypes.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Components/MeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SplineComponent.h"
#include "Engine/World.h"
#include "HAL/CriticalSection.h"
#include "InputAction.h"
#include "Internationalization/TextPackageNamespaceUtil.h"
#include "Misc/ScopeLock.h"
#include "Misc/ScopeExit.h"
#include "StructUtils/InstancedStruct.h"
#include "StructUtils/PropertyBag.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/MetaData.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

namespace UeAgentAuthoringObjectApply::Private {
using FPropertyValueStorage =
    UeAgentAuthoringPropertyValues::FPropertyValueStorage;

struct FMaterialization {
	TSharedPtr<FNativeOwnedCreationContext> NativeContext;
	FString NativePropertyPath;
	FObjectPropertyBase* NativeScopedProperty = nullptr;
	int32 NativeIndex = INDEX_NONE;
	UE::UAI::HjsonWorkspace::DomainAuthoring::FDomainHjsonCandidateSelector NativeSelector;
  TWeakObjectPtr<UObject> ScratchObject;
  TStrongObjectPtr<UObject> CreatedObject;
  TArray<TSharedPtr<FJsonValue>> SourceProperties;
  bool bHasTextSourceEdits = false;
};

struct FTextSourceEdit {
  FTextProperty* Property = nullptr;
  FText Before;
  FString Source;
  TSharedPtr<FJsonValue> Input;
  TSharedPtr<FJsonObject> Completed;
};

struct FExistingInlinePatch {
  FString PropertyPath;
  TArray<TSharedPtr<FJsonValue>> Properties;
  TSharedPtr<FJsonValue> ReadableProperties;
};

struct FPreparedProperty {
  FString PropertyPath;
  int32 SourceDescriptorIndex = INDEX_NONE;
  UObject *NotificationOwner = nullptr;
  FProperty *Property = nullptr;
  TArray<FProperty *> NotificationChain;
  void *CurrentValuePtr = nullptr;
  TSharedPtr<FPropertyValueStorage> DesiredValue;
  TSharedPtr<FPropertyValueStorage> OriginalValue;
  TUniquePtr<FEnvTraceData> LinkedTraceBefore;
  TSharedPtr<FJsonValue> CompletedValue;
  TMap<const FJsonValue*, TSharedPtr<FPropertyValueStorage>> NativeStructBaselines;
  TArray<FTextSourceEdit> TextSourceEdits;
  TArray<TStrongObjectPtr<UScriptStruct>> DynamicFieldTypes;
  FResolvedNode Node;
  TArray<TUniquePtr<FPreparedApply>> NestedOwnedPlans;
  TArray<FExistingInlinePatch> ExistingInlinePatches;
  bool bDescendantsOnly = false;
  TArray<FMaterialization> Materializations;
  int32 NestedPlansAttempted = 0;
  bool bPropertyApplied = false;
};

struct FFrozenPropertyValue {
  FString Path;
  TSharedPtr<FJsonObject> Type;
  TSharedPtr<FJsonValue> Value;
  TSharedPtr<FJsonObject> Source;
};

struct FFrozenOwnedIntent {
  FString RelativePath;
  FPropertyIntent Intent;
};

struct FPrepareSession {
	FArrayProperty* OwnedArray = nullptr;
	FString OwnedArrayPath;
	int32 OwnedIndex = INDEX_NONE;
  TSet<const UObject *> ObjectStack;
  bool bDeferTextSourceEdits = false;
  bool bUniqueDescriptorOccurrences = false;
  bool bCollectValueDiagnostics = false;
  int32 PreparationDepth = 0;
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
  TArray<Private::FFrozenPropertyValue> UnchangedValues;
  TArray<Private::FFrozenOwnedIntent> UnchangedOwnedIntents;
  TArray<FResolvedNode> Nodes;
  TArray<TSharedPtr<FJsonValue>> SourceDescriptors;
  TArray<TUniquePtr<FPreparedApply>> StageStorage;
  bool bBatchNotifications = false;
  bool bWholeObjectNotification = false;
  UeAgentAuthoringNativeDefaults::FScratchScope Scratch;
  UPackage *ScratchPackage = nullptr;
  TMap<UPackage *, bool> PackageDirtyStates;
  UeAgentTemplatePropertyPropagation::FSnapshot InheritedInstanceValues;
  TMap<USplineComponent*, bool> SplineEditedBefore;
  TMap<UPackage *, TMap<FName, TOptional<FString>>> TextMetadataBefore;
  UPackage* TextIdentityPackage = nullptr;
  bool bDeferTextSourceEdits = false;
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

struct FPropertyIntent::FImpl {
  FString CapturedOwnerPath;
  FString DiagnosticTargetPath;
  bool bAllowReadOnly = false;
  TArray<Private::FFrozenPropertyValue> Values;
  TArray<Private::FFrozenOwnedIntent> Owned;
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

FDiagnostic PropertyFailure(const FString& Code, const FString& PublicReason,
    const FString& PropertyPath = FString(), const TSharedPtr<FJsonValue>& Actual = nullptr,
    const TSharedPtr<FJsonValue>& Expected = nullptr, const FString& ValuePointer = FString()) {
  auto Diagnostic = FDiagnostic::Failure(Code, PublicReason, TEXT("object_apply_preflight"));
  Diagnostic.PropertyPath = PropertyPath;
  Diagnostic.ActualValue = Actual;
  Diagnostic.ExpectedValue = Expected;
  if (Actual || Expected) Diagnostic.ActualValuePresent = Actual.IsValid();
  Diagnostic.Source.InternalJsonPath = ValuePointer;
  return Diagnostic;
}

void SupplementLogicalTarget(FDiagnostic& Diagnostic, const FString& LogicalTarget, const FString& NativeOwnerPath) {
  if (LogicalTarget.IsEmpty()) return;
  for (auto& Child : Diagnostic.Diagnostics) SupplementLogicalTarget(Child, LogicalTarget, NativeOwnerPath);
  if (Diagnostic.PublicCause) {
    auto Cause = MakeShared<FDiagnostic>(*Diagnostic.PublicCause);
    SupplementLogicalTarget(*Cause, LogicalTarget, NativeOwnerPath);
    Diagnostic.PublicCause = MoveTemp(Cause);
  }
  // Only the declared owner identity is projected; explicit targets of referenced objects survive.
  if (!Diagnostic.IsEmpty() && (Diagnostic.TargetPath.IsEmpty() || Diagnostic.TargetPath == NativeOwnerPath))
    Diagnostic.TargetPath = LogicalTarget;
}

void SetFailure(FFailure& OutFailure, FDiagnostic Diagnostic,
    const FString& InternalContext = FString(), const FString& PropertyPath = FString()) {
  if (Diagnostic.IsEmpty()) Diagnostic = FDiagnostic::Failure(TEXT("property_diagnostic_missing"),
      TEXT("The property operation failed without reporting its diagnostic."), TEXT("object_apply_preflight"));
  if (Diagnostic.Stage.IsEmpty()) Diagnostic.Stage = TEXT("object_apply_preflight");
  if (Diagnostic.PropertyPath.IsEmpty()) Diagnostic.PropertyPath = PropertyPath;
  if (Diagnostic.InternalContext.IsEmpty()) Diagnostic.InternalContext = InternalContext;
  OutFailure.Diagnostic = MoveTemp(Diagnostic);
}

// The resolved tree records authored members, before completion adds preserved
// native values. Reuse that boundary for writes and verification. Containers,
// specialized structs and references retain their existing whole-value contract.
bool CopyExplicitPropertyValue(FProperty* Property, const void* Desired, void* Target,
    const FResolvedNode& Node, FDiagnostic& Error) {
  if (Node.ValueKind != TEXT("struct")) {
    Property->CopyCompleteValue(Target, Desired);
    return true;
  }
  const auto* Struct = CastField<FStructProperty>(Property);
  if (!Struct || Property->ArrayDim != 1) {
    Error = FDiagnostic::Failure(TEXT("property_intent_structure_changed"), TEXT("The property no longer has the single struct shape captured for this edit."), TEXT("object_apply")); return false;
  }
  for (const auto& Child : Node.Children) {
    if (!Child.bExplicit) continue;
    const FString Name = Child.PropertyPath.RightChop(Node.PropertyPath.Len() + 1);
    FProperty* Field = FindFProperty<FProperty>(Struct->Struct, *Name);
    if (!Field) { Error = FDiagnostic::Failure(TEXT("property_intent_field_missing"), TEXT("A field captured in the property intent no longer exists in the native struct."), TEXT("object_apply")); return false; }
    if (!CopyExplicitPropertyValue(Field, Field->ContainerPtrToValuePtr<void>(Desired),
        Field->ContainerPtrToValuePtr<void>(Target), Child, Error)) return false;
  }
  if (UeAgentEnvTraceDataValue::IsType(Struct->Struct)) {
    const bool bChannelAuthored = Node.Children.ContainsByPredicate([&](const FResolvedNode& Child) {
      if (!Child.bExplicit) return false;
      const FString Name = Child.PropertyPath.RightChop(Node.PropertyPath.Len() + 1);
      return Name == TEXT("TraceChannel") || Name == TEXT("SerializedChannel");
    });
    auto& Trace = *static_cast<FEnvTraceData*>(Target);
    if (bChannelAuthored)
      UeAgentEnvTraceDataValue::CopyChannels(*static_cast<const FEnvTraceData*>(Desired), Trace);
    else
      Trace.OnPostLoad();
  }
  return true;
}

TSharedPtr<FJsonValue> ProjectExplicitIntentValue(const FResolvedNode& Node,
    const TSharedPtr<FJsonValue>& Encoded) {
  if (Node.ValueKind != TEXT("struct")) return Encoded;
  const TSharedPtr<FJsonObject>* EncodedFields = nullptr;
  if (!Encoded || Encoded->Type != EJson::Object
      || !Encoded->AsObject()->TryGetObjectField(TEXT("fields"), EncodedFields)) return nullptr;
  const auto Fields = MakeShared<FJsonObject>();
  for (const auto& Child : Node.Children) {
    if (!Child.bExplicit) continue;
    const FString Name = Child.PropertyPath.RightChop(Node.PropertyPath.Len() + 1);
    const auto Value = ProjectExplicitIntentValue(Child, (*EncodedFields)->TryGetField(Name));
    if (!Value) return nullptr;
    Fields->SetField(Name, Value);
  }
  const auto Value = MakeShared<FJsonObject>();
  Value->Values = Encoded->AsObject()->Values;
  Value->SetObjectField(TEXT("fields"), Fields);
  return MakeShared<FJsonValueObject>(Value);
}

// Preserve authored members and the native changes caused by them. Replacing a
// whole completed struct would also freeze every unrelated baseline member;
// keeping only authored members would lose coupled native effects needed when a
// normalized driver (for example a profile) is rewritten by its native owner.
TSharedPtr<FJsonValue> ProjectNativeNormalizedIntentValue(const FResolvedNode& Node,
    FProperty* Property, const void* Desired, const void* Native, UObject* Owner,
    const TSharedPtr<FJsonValue>& Encoded, FDiagnostic& Error) {
  if (Node.ValueKind != TEXT("struct")) return Encoded;
  const auto* Struct = CastField<FStructProperty>(Property);
  const TSharedPtr<FJsonObject>* EncodedFields = nullptr;
  if (!Struct || Property->ArrayDim != 1 || !Encoded || Encoded->Type != EJson::Object
      || !Encoded->AsObject()->TryGetObjectField(TEXT("fields"), EncodedFields)) {
    Error = FDiagnostic::Failure(TEXT("native_normalization_capture_failed"), TEXT("The native normalized property value could not be captured."), TEXT("object_apply")); return nullptr;
  }
  const auto Fields = MakeShared<FJsonObject>();
  for (const auto& FieldValue : (*EncodedFields)->Values) {
    FProperty* Field = FindFProperty<FProperty>(Struct->Struct, *FieldValue.Key);
    if (!Field) { Error = FDiagnostic::Failure(TEXT("property_intent_field_missing"), TEXT("A field captured in the property intent no longer exists in the native struct."), TEXT("object_apply")); return nullptr; }
    const FString Path = Node.PropertyPath + TEXT(".") + FieldValue.Key;
    const auto* Authored = Node.Children.FindByPredicate([&](const auto& Child) {
      return Child.bExplicit && Child.PropertyPath == Path;
    });
    const void* Before = Field->ContainerPtrToValuePtr<void>(Desired);
    const void* After = Field->ContainerPtrToValuePtr<void>(Native);
    if (!Authored) {
      bool bEquivalent = false;
      if (!UeAgentAuthoringSemanticCompare::ArePropertyValuesEquivalent(
          Field, Before, After, Owner, bEquivalent, Error)) return nullptr;
      if (bEquivalent) continue;
    }
    FResolvedNode Child;
    if (Authored) Child = *Authored;
    else {
      Child.PropertyPath = Path;
      Child.ValueKind = UeAgentAuthoringTypeDescriptors::GetValueKind(Field, false);
    }
    const auto Value = ProjectNativeNormalizedIntentValue(
        Child, Field, Before, After, Owner, FieldValue.Value, Error);
    if (!Value) return nullptr;
    Fields->SetField(FieldValue.Key, Value);
  }
  const auto Value = MakeShared<FJsonObject>();
  Value->Values = Encoded->AsObject()->Values;
  Value->SetObjectField(TEXT("fields"), Fields);
  return MakeShared<FJsonValueObject>(Value);
}

const TSet<FString> &PropertyDescriptorFields() {
  static const TSet<FString> Fields = {
      TEXT("property_path"), TEXT("value"),        TEXT("kind"),
      TEXT("struct_path"),   TEXT("enum_path"),    TEXT("element_type"),
      TEXT("key_type"),      TEXT("mapped_type"),  TEXT("inner_type"),
      TEXT("fixed_length"),  TEXT("allowed_class"), TEXT("hash_source"), TEXT("authoring_source")};
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
    return UeAgentAuthoringState::CloneCanonicalValue(Value);
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
                       FDiagnostic& OutReason) {
  OutReason.Reset();
  if (!Object || !Object->GetClass() || !Object->GetPackage()) {
    OutReason = FDiagnostic::Failure(TEXT("invalid_owner"), TEXT("The property owner, its class, or its package is unavailable."), TEXT("object_apply"));
    return false;
  }
  if (!Options.bAllowTemplateOwners &&
      Object->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject)) {
    OutReason = FDiagnostic::Failure(TEXT("template_owner_requires_domain_authorization"), TEXT("Editing a class default or archetype object requires the owning domain to authorize that target."), TEXT("object_apply"));
    return false;
  }
  if (const UWorld *World = Object->GetWorld();
      World && World->WorldType == EWorldType::PIE) {
    OutReason = FDiagnostic::Failure(TEXT("pie_runtime_owner_requires_runtime_command"), TEXT("A PIE runtime object must be edited through a runtime command."), TEXT("object_apply"));
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
  OutReason = FDiagnostic::Failure(PackageName.StartsWith(TEXT("/Engine/"))
                  ? TEXT("engine_content_owner_is_read_only")
                  : TEXT("non_game_mount_owner_requires_domain_authorization"),
      PackageName.StartsWith(TEXT("/Engine/")) ? TEXT("Engine content is read-only through property authoring.")
          : TEXT("Editing this mounted package requires authorization from its owning domain."), TEXT("object_apply"));
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
                        const FOptions &Options, FDiagnostic& OutReason, const UStruct* Scope = nullptr) {
  const FProperty* Declaration = Property ? Property->GetOwnerProperty() : nullptr;
  if (!Declaration || IsDangerousProperty(Property)) {
    OutReason = FDiagnostic::Failure(TEXT("transient_deprecated_or_unsafe_property"), TEXT("The property is unavailable, transient, deprecated, or excluded from persistent authoring."), TEXT("object_apply"));
    return false;
  }
  Property = Declaration;
  if (Options.IncludeProperty && !Options.IncludeProperty(Property)) {
    OutReason = FDiagnostic::Failure(TEXT("property_outside_domain_scope"), TEXT("The property is outside the fields owned by this domain."), TEXT("object_apply"));
    return false;
  }
  if (IsDangerousProperty(Property)) {
    OutReason = FDiagnostic::Failure(TEXT("transient_deprecated_or_unsafe_property"), TEXT("The property is unavailable, transient, deprecated, or excluded from persistent authoring."), TEXT("object_apply"));
    return false;
  }
  if (!Options.bAllowReadOnly &&
      !UeAgentAuthoringReflection::IsEditableByFlags(Property, Owner, Options.bShowDefaultsOnly, Options.bUseInstanceEditability, Scope)
      && !(Options.AuthorizeProperty && Options.AuthorizeProperty(Property)
          && !Property->HasAnyPropertyFlags(CPF_EditConst))) {
    OutReason = FDiagnostic::Failure(TEXT("read_only_or_non_editable"), TEXT("The native property is read-only or not editable in this context."), TEXT("object_apply"));
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
  SetFailure(OutFailure, PropertyFailure(ErrorCode, TEXT("The property value must be an object."), PropertyPath), ErrorCode);
  OutFailure.Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("object"));
  OutFailure.Diagnostic.ActualValue = Value;
  OutFailure.Diagnostic.ActualValuePresent = Value.IsValid();
  OutFailure.Diagnostic.Source.InternalJsonPath = TEXT("/");
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
    SetFailure(OutFailure, PropertyFailure(ErrorPrefix, TEXT("The value contains a field that is not accepted by this property shape."), PropertyPath + TEXT(".") + Field), Field);
    TArray<TSharedPtr<FJsonValue>> Choices;
    for (const TCHAR* Name : AllowedFields) Choices.Add(MakeShared<FJsonValueString>(Name));
    OutFailure.Diagnostic.ExpectedValue = MakeShared<FJsonValueArray>(Choices);
    OutFailure.Diagnostic.ActualValue = MakeShared<FJsonValueString>(Field);
    OutFailure.Diagnostic.Source.InternalJsonPath = TEXT("/") + Field.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
    return false;
  }
  return true;
}

UeAgentPropertyValueCodec::FOptions MakeCodecOptions(const FOptions &Options, bool bStoredValues = false) {
  UeAgentPropertyValueCodec::FOptions Result;
  Result.bCompareStoredValues = bStoredValues;
  if (!Options.bAllowReadOnly) {
    // Native storage already carries the baseline. Do not make an opaque,
    // noneditable member a prerequisite for editing the other struct fields.
    // Supported read-only members retain the existing completed-value shape.
    Result.StructFieldFilter = [](const UScriptStruct* Struct, const FProperty* Field) {
      FDiagnostic Reason;
      return UeAgentPropertyValueCodec::IsStructFieldAuthoringEditable(Field, Struct)
          || UeAgentPropertyValueCodec::IsAuthoringSupported(Field, nullptr, Reason);
    };
  }
  return Result;
}

bool ResolveHashSource(FProperty* Property, const TSharedPtr<FJsonValue>& From,
    const TArray<const void*>& Candidates, UObject* Owner, const FOptions& Options,
    const FString& Path, int32& OutIndex, FFailure& Failure) {
  OutIndex = INDEX_NONE;
  if (!From) { SetFailure(Failure, PropertyFailure(TEXT("hash_source_required"), TEXT("A partially editable set item or map key requires a from value to identify its captured source."), Path, nullptr, MakeShared<FJsonValueString>(TEXT("captured from value or null for a new entry")), TEXT("/from")), FString()); return false; }
  if (From->Type == EJson::Null) return true;
  auto Codec = MakeCodecOptions(Options);
  Codec.bAllowPartialStructFields = false;
  Codec.StructFieldFilter = nullptr; // Hash identity uses complete native facts.
  Codec.bCompareStoredValues = true;
  int32 NativeMatches = 0;
  for (int32 Index = 0; Index < Candidates.Num(); ++Index) {
    FDiagnostic Error;
    auto Probe = FPropertyValueStorage::CopyFrom(Property, Candidates[Index], Error);
    if (!Probe || !UeAgentPropertyValueCodec::Decode(Property, From, Probe->GetMutableValuePtr(), Owner, Codec, Error)) {
      SetFailure(Failure, Error, FString(), Path); return false;
    }
    if (!Property->Identical(Probe->GetValuePtr(), Candidates[Index], PPF_None)) continue;
    ++NativeMatches;
    const auto Compared = UeAgentPropertyValueCodec::ComparePropertyValues(
        Property, Probe->GetValuePtr(), Candidates[Index], Owner, Codec);
    if (!Compared.IsComparable()) {
      Compared.WithPrefix(TEXT("/from")).ApplyTo(Error);
      SetFailure(Failure, Error, FString(), Path); return false;
    }
    if (Compared.bEquivalent) OutIndex = Index;
  }
  if (NativeMatches == 1 && OutIndex != INDEX_NONE) return true;
  SetFailure(Failure, PropertyFailure(NativeMatches > 1 ? TEXT("hash_source_ambiguous")
      : NativeMatches == 1 ? TEXT("hash_source_facts_mismatch") : TEXT("hash_source_not_found"), NativeMatches > 1 ? TEXT("The from value matches more than one captured key.") : NativeMatches == 1 ? TEXT("The from value has the same native key but different captured facts.") : TEXT("The from value does not match a captured key."), Path, From, MakeShared<FJsonValueString>(TEXT("exactly one captured key with matching facts")), TEXT("/from")), FString());
  return false;
}

bool ValidateCompletedHashIdentity(FProperty* Property, const TSharedPtr<FJsonValue>& Completed,
    const TArray<const void*>& Candidates, int32 SourceIndex, UObject* Owner,
    const FOptions& Options, const FString& Path, FFailure& Failure) {
  FDiagnostic Error;
  auto Value = FPropertyValueStorage::CreateInitialized(Property, Error);
  auto Codec = MakeCodecOptions(Options);
  Codec.StructFieldFilter = nullptr;
  if (!Value || !UeAgentPropertyValueCodec::Decode(Property, Completed, Value->GetMutableValuePtr(), Owner, Codec, Error)) {
    SetFailure(Failure, Error, FString(), Path); return false;
  }
  if (SourceIndex != INDEX_NONE) {
    if (Property->Identical(Value->GetValuePtr(), Candidates[SourceIndex], PPF_None)) return true;
    auto Diagnostic = PropertyFailure(TEXT("hash_identity_changed"), TEXT("The edit changes the identity of an existing set item or map key."), Path);
    UeAgentPropertyValueCodec::ComparePropertyValues(Property, Candidates[SourceIndex],
        Value->GetValuePtr(), Owner, Codec).ApplyTo(Diagnostic);
    SetFailure(Failure, MoveTemp(Diagnostic)); return false;
  }
  for (const void* Candidate : Candidates)
    if (Property->Identical(Value->GetValuePtr(), Candidate, PPF_None)) {
      SetFailure(Failure, PropertyFailure(TEXT("hash_source_existing_requires_from"), TEXT("The proposed set item or map key already exists and must identify that captured source with from."), Path, Completed, MakeShared<FJsonValueString>(TEXT("from identifying the existing key"))), FString()); return false;
    }
  return true;
}

bool CaptureNativeStructBaseline(FProperty* Property, const void* Base,
    const TSharedPtr<FJsonValue>& Completed, bool bSingleElement,
    FPreparedProperty& Entry, const FString& Path, FFailure& Failure) {
  FDiagnostic Reason;
  if (!Base || !Completed || (!(bSingleElement && Property->ArrayDim > 1)
      && UeAgentPropertyValueCodec::IsSupported(Property, Base, Reason))) return true;
  auto Storage = FPropertyValueStorage::CreateInitialized(Property, Reason);
  if (!Storage) { SetFailure(Failure, Reason, FString(), Path); return false; }
  if (bSingleElement) Property->CopySingleValue(Storage->GetMutableValuePtr(), Base);
  else Property->CopyCompleteValue(Storage->GetMutableValuePtr(), Base);
  Entry.NativeStructBaselines.Add(Completed.Get(), MoveTemp(Storage));
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
                         FResolvedNode &OutNode, FFailure &OutFailure);

bool ResolveReadableOwnedProperties(UObject* NativeObject,
    const TSharedPtr<FJsonValue>& ReadableProperties, const FOptions& Options,
    const FString& PropertyPath, TArray<TSharedPtr<FJsonValue>>& Properties,
    FFailure& OutFailure) {
    if (!ReadableProperties.IsValid()) return true;
    UeAgentDetailsAuthoringService::FCapturePolicy Policy;
    Policy.bShowDefaultsOnly = Options.bShowDefaultsOnly;
    Policy.bUseInstanceEditability = Options.bUseInstanceEditability;
    UeAgentAuthoringState::FStateSnapshot Snapshot;
    TSharedPtr<FJsonObject> Document;
    FDiagnostic Error;
    if (!UeAgentDetailsAuthoringService::CaptureObjectProperties(NativeObject, Policy, Snapshot, Document, Error)
        || !UeAgentDetailsAuthoringService::DecodeReadableProperties(Snapshot, ReadableProperties, Properties, Error)) {
      SetFailure(OutFailure, MoveTemp(Error), FString(), PropertyPath);
      return false;
    }
    return true;
}

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
    SetFailure(OutFailure, PropertyFailure(TEXT("owned_object_hash_key_forbidden"), TEXT("An owned object cannot be created or edited as a set item or map key."), PropertyPath), FString());
    return false;
  }
  if (!HasOnlyFields(InputObject, {TEXT("owned_object")}, OutFailure, PropertyPath, TEXT("owned_object_unknown_field"))) return false;
  if (!HasOnlyFields(*OwnedObject, {TEXT("class_path"), TEXT("properties"), TEXT("owner_relative_path"), TEXT("name"), TEXT("select")},
                     OutFailure, PropertyPath,
                     TEXT("owned_object_unknown_field"))) {
    UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(OutFailure.Diagnostic, TEXT("/owned_object"));
    return false;
  }

  FString ExplicitClassPath;
  FDiagnostic MetadataError;
  if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(**OwnedObject,
          {{TEXT("class_path"), EJson::String}}, MetadataError, TEXT("/owned_object"))) {
    SetFailure(OutFailure, MetadataError, FString(), PropertyPath);
    OutFailure.Diagnostic.PropertyPath = PropertyPath;
    return false;
  }
  (*OwnedObject)->TryGetStringField(TEXT("class_path"), ExplicitClassPath);
  const TArray<TSharedPtr<FJsonValue>> *NestedProperties = nullptr;
  const auto ReadableProperties = (*OwnedObject)->TryGetField(TEXT("properties"));
  const bool bReadableProperties = ReadableProperties && ReadableProperties->Type == EJson::Object && Options.bCompactValues;
  if ((*OwnedObject)->HasField(TEXT("properties")) &&
      !bReadableProperties &&
      (!(*OwnedObject)
            ->TryGetArrayField(TEXT("properties"), NestedProperties) ||
       !NestedProperties)) {
    SetFailure(OutFailure, PropertyFailure(TEXT("owned_object_properties_invalid"), TEXT("Owned object properties must be a descriptor array or supported compact property object."), PropertyPath, ReadableProperties, MakeShared<FJsonValueString>(TEXT("properties array or compact object")), TEXT("/owned_object/properties")), FString());
    return false;
  }
  TArray<TSharedPtr<FJsonValue>> Properties = NestedProperties ? *NestedProperties : TArray<TSharedPtr<FJsonValue>>();


  UObject *ExistingObject =
      ObjectProperty->GetObjectPropertyValue(BaseValuePtr);
  FDiagnostic ReplacementReason;
  const bool bDescendantsOnly =
      !UeAgentAuthoringReflection::IsOwnedObjectProperty(ObjectProperty)
      || !IsEditableForApply(ObjectProperty->GetOwnerProperty(), Owner, Options, ReplacementReason);
  if (bDescendantsOnly && (!ExistingObject
      || !UeAgentAuthoringReflection::IsExistingInlineObjectProperty(
          ObjectProperty, BaseValuePtr, Owner))) {
    SetFailure(OutFailure, PropertyFailure(TEXT("existing_inline_object_required"), TEXT("A recursive edit requires an existing inline object at this property."), PropertyPath), FString());
    return false;
  }
  if ((*OwnedObject)->HasField(TEXT("owner_relative_path"))) {
    FString RelativePath;
    if (!(*OwnedObject)->TryGetStringField(TEXT("owner_relative_path"), RelativePath)) {
      SetFailure(OutFailure, PropertyFailure(TEXT("owned_object_binding_invalid"), TEXT("The owned object binding must be a string relative to its owner."), PropertyPath), FString());
      return false;
    }
    if (RelativePath.IsEmpty() && bDescendantsOnly) {
      SetFailure(OutFailure, PropertyFailure(TEXT("existing_inline_pointer_replacement_forbidden"), TEXT("This property permits editing the existing inline object, but not replacing its pointer."), PropertyPath), FString());
      return false;
    }
    if (RelativePath.IsEmpty()) ExistingObject = nullptr;
    else if (!ExistingObject || !Owner || ExistingObject->GetPathName(Owner) != RelativePath) {
      SetFailure(OutFailure, PropertyFailure(TEXT("owned_object_binding_not_found"), TEXT("The requested owned object binding does not identify an object of the required class within this owner."), PropertyPath), FString());
      return false;
    }
  }
  UClass *ObjectClass = ExistingObject ? ExistingObject->GetClass() : nullptr;
	TSharedPtr<FNativeOwnedCreationContext> NativeContext;
	FString NativePropertyPath = Entry.Property == ObjectProperty ? Entry.PropertyPath : FString();
	FObjectPropertyBase* NativeScopedProperty = nullptr;
	if (!Prepared.RootObject && !Options.StructNotificationOwnerPropertyPath.IsEmpty()) {
		NativePropertyPath = Options.StructNotificationOwnerPropertyPath;
		NativeScopedProperty = ObjectProperty;
	}
	int32 NativeIndex = INDEX_NONE;
	if (Session.OwnedArray) {
		if (!NativeScopedProperty) NativePropertyPath = Session.OwnedArrayPath;
		NativeIndex = Session.OwnedIndex;
	}
	UE::UAI::HjsonWorkspace::DomainAuthoring::FDomainHjsonCandidateSelector Selector;
	if ((*OwnedObject)->HasField(TEXT("name")) || (*OwnedObject)->HasField(TEXT("select"))) {
		if (ExistingObject || !ExplicitClassPath.IsEmpty() || NativePropertyPath.IsEmpty()) {
			SetFailure(OutFailure, PropertyFailure(TEXT("owned_candidate_context_invalid"), TEXT("A creation candidate requires an unresolved owned object without an explicit class and with a native property context."), PropertyPath), FString()); return false;
		}
		auto Candidate = MakeShared<FJsonObject>();
		for (const TCHAR* Key : {TEXT("name"), TEXT("select")}) if (auto Value = (*OwnedObject)->TryGetField(Key)) Candidate->SetField(Key, Value);
		UE::UAI::HjsonWorkspace::FWorkspaceError Error;
		if (!UE::UAI::HjsonWorkspace::DomainAuthoring::TryReadCandidateSelector(Candidate, PropertyPath, Selector, Error)) {
			SetFailure(OutFailure, Error.GetDiagnostic(), FString(), PropertyPath); return false;
		}
		NativeContext = MakeShared<FNativeOwnedCreationContext>();
		FDiagnostic Reason;
		TArray<FOwnedClassCandidate> Candidates;
		const bool bInitialized = NativeScopedProperty
			? NativeContext->InitializeScopedMember(Owner, NativePropertyPath, NativeScopedProperty, true, Reason)
			: NativeContext->Initialize(Owner, NativePropertyPath, true, Reason, NativeIndex);
		if (!bInitialized || !NativeContext->Collect(Candidates, Reason)) {
			SetFailure(OutFailure, Reason, PropertyPath, PropertyPath); return false;
		}
		TArray<UE::UAI::ContentDiscovery::FNameSelectionEntry> Names;
		for (const auto& Item : Candidates) Names.Add(Item.Selection);
		int32 Selected;
		if (!UE::UAI::ContentDiscovery::TryResolveNameSelection(Names, Selector.Name, Selector.Select, Selected, Reason)) {
			SetFailure(OutFailure, Reason, FString(), PropertyPath); return false;
		}
		ObjectClass = Candidates[Selected].Class;
	}
  if (NativeContext) {
    // The native picker has resolved the exact class in the prospective owner.
  } else if (!ExplicitClassPath.IsEmpty()) {
    UClass *ExplicitClass = LoadObject<UClass>(nullptr, *ExplicitClassPath);
    if (!ExplicitClass) {
      SetFailure(OutFailure, PropertyFailure(TEXT("owned_object_class_not_found"), TEXT("The requested owned object class could not be loaded."), PropertyPath, MakeShared<FJsonValueString>(ExplicitClassPath), MakeShared<FJsonValueString>(TEXT("loadable class")), TEXT("/owned_object/class_path")), ExplicitClassPath);
      return false;
    }
    if (!ObjectProperty->PropertyClass ||
        !ExplicitClass->IsChildOf(ObjectProperty->PropertyClass)) {
      SetFailure(OutFailure, PropertyFailure(TEXT("owned_object_class_not_allowed"), TEXT("The requested class is not derived from the class accepted by this property."), PropertyPath, MakeShared<FJsonValueString>(ExplicitClassPath), MakeShared<FJsonValueString>(ObjectProperty->PropertyClass ? ObjectProperty->PropertyClass->GetPathName() : FString()), TEXT("/owned_object/class_path")), ExplicitClassPath);
      return false;
    }
    if (ExistingObject && ExistingObject->GetClass() != ExplicitClass) {
      SetFailure(OutFailure, PropertyFailure(TEXT("owned_object_existing_class_conflict"), TEXT("The requested class differs from the existing owned object class."), PropertyPath, MakeShared<FJsonValueString>(ExplicitClassPath), MakeShared<FJsonValueString>(ExistingObject->GetClass()->GetPathName()), TEXT("/owned_object/class_path")), ExplicitClassPath);
      return false;
    }
    ObjectClass = ExplicitClass;
  } else if (!ExistingObject) {
    UClass *DeclaredClass = ObjectProperty->PropertyClass.Get();
    if (!DeclaredClass ||
        DeclaredClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated |
                                        CLASS_NewerVersionExists)) {
      SetFailure(OutFailure, PropertyFailure(TEXT("owned_object_class_source_required"), TEXT("The declared property class cannot be instantiated; select a concrete class."), PropertyPath), FString());
      return false;
    }
    ObjectClass = DeclaredClass;
  }
  if (!ObjectClass ||
      ObjectClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated |
                                    CLASS_NewerVersionExists)) {
    SetFailure(OutFailure, PropertyFailure(TEXT("owned_object_class_invalid"), TEXT("The resolved owned object class is missing, abstract, deprecated, or superseded."), PropertyPath), FString());
    return false;
  }

  OutNode.ValueKind = TEXT("owned_object");
  OutNode.TypePath = ObjectClass->GetPathName();
  if (ExistingObject) {
    if (!Owner || !IsObjectWithin(ExistingObject, Owner)) {
      SetFailure(OutFailure, PropertyFailure(TEXT("owned_object_outer_invalid"), TEXT("The existing owned object is outside the requested owner."), PropertyPath), ExistingObject->GetPathName());
      return false;
    }
    if (Session.ObjectStack.Contains(ExistingObject)) {
      SetFailure(OutFailure, PropertyFailure(TEXT("object_identity_cycle"), TEXT("Recursive owned object editing revisited an object already being prepared."), PropertyPath), ExistingObject->GetPathName());
      return false;
    }
    if (bDescendantsOnly) {
      // This entry has no pointer-write authority. Its native owner may derive
      // a new instance while applying the other root fields.
      Entry.bDescendantsOnly = Entry.Property == ObjectProperty && Entry.PropertyPath == PropertyPath;
      FExistingInlinePatch& Patch = Entry.ExistingInlinePatches.AddDefaulted_GetRef();
      Patch.PropertyPath = PropertyPath;
      Patch.Properties = Properties;
      Patch.ReadableProperties = bReadableProperties ? ReadableProperties : nullptr;
    } else {
      if (!ResolveReadableOwnedProperties(ExistingObject, bReadableProperties ? ReadableProperties : nullptr,
          Options, PropertyPath, Properties, OutFailure)) return false;
      if (!Properties.IsEmpty()) {
        // Every existing owned occurrence keeps its own plan. Array/map entries
        // must not overwrite another occurrence's child edits or rollback state.
        TUniquePtr<FPreparedApply> Nested = MakeUnique<FPreparedApply>();
        if (!PrepareRootInternal(ExistingObject->GetClass(), ExistingObject, ExistingObject,
            ExistingObject, ExistingObject->GetPathName(), Properties, Options, Session,
            *Nested, OutFailure)) return false;
        OutNode.Children = Nested->GetResolvedNodes();
        if (Nested->NumProperties() > 0)
          Entry.NestedOwnedPlans.Add(MoveTemp(Nested));
        else {
          auto& Intent = Prepared.UnchangedOwnedIntents.AddDefaulted_GetRef();
          Intent.RelativePath = ExistingObject->GetPathName(Prepared.NotificationOwner);
          if (!Nested->CaptureUnchangedIntent(Intent.Intent, OutFailure)) return false;
        }
      }
    }
    OutCompletedValue = MakeObjectPathValue(ExistingObject);
    return true;
  }

  FDiagnostic ScratchError;
  UPackage *ScratchPackage = Prepared.ScratchPackage;
  if (!ScratchPackage) {
    ScratchPackage = Prepared.Scratch.CreateScratchPackage(ScratchError);
    Prepared.ScratchPackage = ScratchPackage;
  }
  if (!ScratchPackage) {
    SetFailure(OutFailure, ScratchError, FString(), PropertyPath);
    return false;
  }
  UObject *ScratchObject = NativeContext ? NativeContext->Create(ObjectClass, ScratchError)
      : Prepared.Scratch.CreateObject(ObjectClass, ScratchPackage, nullptr, ScratchError);
  if (!ScratchObject || !ScratchError.IsEmpty()) {
    SetFailure(OutFailure, ScratchError, FString(), PropertyPath);
    return false;
  }
  if (!ResolveReadableOwnedProperties(ScratchObject, bReadableProperties ? ReadableProperties : nullptr, Options, PropertyPath, Properties, OutFailure)) return false;

  bool bHasDeferredTextSources = false;
  if (!Properties.IsEmpty()) {
    FOptions ScratchOptions = Options;
    ScratchOptions.bEmitEditorNotifications = false;
    ScratchOptions.bMarkPackageDirty = false;
    ScratchOptions.bAllowTemplateOwners = true;
    FPreparedApply ScratchPlan;
    TGuardValue<bool> DeferTextSources(Session.bDeferTextSourceEdits, true);
    if (!PrepareRootInternal(
            ScratchObject->GetClass(), ScratchObject, ScratchObject,
            ScratchObject, ScratchObject->GetPathName(), Properties,
            ScratchOptions, Session, ScratchPlan, OutFailure)) {
      return false;
    }
    FApplyResult ScratchResult;
    if (!ApplyPrepared(ScratchPlan, ScratchResult)) {
      SetFailure(OutFailure, ScratchResult.Diagnostic, FString(), PropertyPath);
      return false;
    }
    OutNode.Children = ScratchPlan.GetResolvedNodes();
    bHasDeferredTextSources = ScratchPlan.HasTextSourceEdits();
  }

  FMaterialization &Materialization =
      Entry.Materializations.AddDefaulted_GetRef();
  Materialization.ScratchObject = ScratchObject;
	Materialization.NativeContext = NativeContext;
	Materialization.NativeSelector = Selector;
	Materialization.NativeScopedProperty = NativeScopedProperty;
	if (NativeContext) { Materialization.NativePropertyPath = NativePropertyPath; Materialization.NativeIndex = NativeIndex; }
  Materialization.SourceProperties = Properties;
  Materialization.bHasTextSourceEdits = bHasDeferredTextSources;
  OutCompletedValue = MakeObjectPathValue(ScratchObject);
  return true;
}

bool BuildCompletedValue(FProperty *Property,
                         const TSharedPtr<FJsonValue> &RawInputValue,
                         const void *BaseValuePtr, UObject *Owner,
                         FPreparedApply::FImpl &Prepared,
                         FPreparedProperty &Entry, const FOptions &Options,
                         bool bIgnoreFixedArray, bool bHashKeyContext,
                         FPrepareSession &Session, const FString &PropertyPath,
                         TSharedPtr<FJsonValue> &OutCompletedValue,
                         FResolvedNode &OutNode, FFailure &OutFailure) {
  UeAgentAuthoringDiagnostics::FDiagnostic ChildFailures;
  TSharedPtr<FJsonValue> InputValue = RawInputValue;
  OutCompletedValue.Reset();
  OutNode = FResolvedNode();
  OutNode.PropertyPath = PropertyPath;
  OutNode.ValueKind = UeAgentAuthoringTypeDescriptors::GetValueKind(
      Property, bIgnoreFixedArray);
  OutNode.TypePath = PropertyTypePath(Property);
  if (!Property || !InputValue.IsValid()) {
    SetFailure(OutFailure, PropertyFailure(TEXT("invalid_apply_value"), TEXT("The property edit is missing its input value."), PropertyPath, InputValue, MakeShared<FJsonValueString>(TEXT("property value")), TEXT("/")), FString());
    return false;
  }
  if (Options.bCompactValues) {
    if (FTextProperty* TextProperty = CastField<FTextProperty>(Property);
        TextProperty && (bIgnoreFixedArray || Property->ArrayDim == 1)) {
      FString Source;
      const auto Object = InputValue->Type == EJson::Object ? InputValue->AsObject() : nullptr;
      const bool bSourceEdit = InputValue->TryGetString(Source)
          || (Object.IsValid() && Object->Values.Num() == 1
              && Object->TryGetStringField(TEXT("literal"), Source));
      if (bSourceEdit) {
        const FText Before = BaseValuePtr ? TextProperty->GetPropertyValue(BaseValuePtr) : FText();
        const FTextId BeforeId = FTextInspector::GetTextId(Before);
        const FString* BeforeSource = FTextInspector::GetSourceString(Before);
        if ((Source.IsEmpty() && Before.IsEmpty())
            || (!Before.IsCultureInvariant() && !Before.IsFromStringTable()
                && !BeforeId.GetKey().IsEmpty() && BeforeSource && BeforeSource->Equals(Source, ESearchCase::CaseSensitive))) {
          OutCompletedValue = UeAgentPropertyValueCodec::Encode(Property, BaseValuePtr, Owner, MakeCodecOptions(Options), 0, nullptr, true);
          return true;
        }
        FTextSourceEdit& Edit = Entry.TextSourceEdits.AddDefaulted_GetRef();
        Edit.Property = TextProperty;
        Edit.Before = Before;
        Edit.Source = MoveTemp(Source);
        Edit.Input = RawInputValue;
        Edit.Completed = MakeShared<FJsonObject>();
        Edit.Completed->SetStringField(TEXT("literal"), Edit.Source);
        OutCompletedValue = MakeShared<FJsonValueObject>(Edit.Completed);
        return true;
      }
      // Explicit text representations retain their exact namespace/key contract.
      OutCompletedValue = RawInputValue;
      return true;
    }
  }
  if (Options.bCompactValues) {
    FString Wrapper;
    if ((!bIgnoreFixedArray && Property->ArrayDim > 1) ||
        CastField<FArrayProperty>(Property) || CastField<FSetProperty>(Property)) {
      Wrapper = TEXT("items");
    } else {
      const FStructProperty* Struct = CastField<FStructProperty>(Property);
      const FString StructKind = Struct
          ? UeAgentAuthoringTypeDescriptors::GetStructKind(Struct->Struct) : FString();
      const auto InputObject = InputValue->Type == EJson::Object ? InputValue->AsObject() : nullptr;
      // Ordinary structs and containers recurse below. Owned requests retain their
      // materialization context; they are not reference-path shorthand.
      const bool bRecursive = CastField<FMapProperty>(Property) || CastField<FOptionalProperty>(Property)
          || (Struct && (StructKind == TEXT("struct") || StructKind == TEXT("instanced_struct")
              || StructKind == TEXT("property_bag")))
          || (InputObject.IsValid() && InputObject->HasField(TEXT("owned_object")));
      if (!bRecursive) {
        TSharedPtr<FPropertyValueStorage> Default;
        const void* ShapeValue = BaseValuePtr;
        if (!ShapeValue) {
          FDiagnostic StorageError;
          Default = FPropertyValueStorage::CreateInitialized(Property, StorageError);
          if (!Default.IsValid()) {
            SetFailure(OutFailure, StorageError, FString(), PropertyPath);
            return false;
          }
          ShapeValue = Default->GetValuePtr();
        }
        const auto Shape = UeAgentPropertyValueCodec::Encode(Property,
            ShapeValue, Owner, MakeCodecOptions(Options),
            0, nullptr, bIgnoreFixedArray);
        if (Shape.IsValid() && Shape->Type == EJson::Object && Shape->AsObject()->Values.Num() == 1)
          for (const auto& Field : Shape->AsObject()->Values) Wrapper = Field.Key;
      }
    }
    const auto Object = InputValue->Type == EJson::Object ? InputValue->AsObject() : nullptr;
    if (UeAgentAuthoringTypeDescriptors::IsDirectPropertyValueWrapper(Wrapper)
        && (!Object.IsValid() || !Object->HasField(Wrapper))) {
      auto Restored = MakeShared<FJsonObject>();
      Restored->SetField(Wrapper, InputValue);
      InputValue = MakeShared<FJsonValueObject>(Restored);
    }
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
        Items->Num() != Property->ArrayDim) {
      if (OutFailure.Diagnostic.Code.IsEmpty())
        SetFailure(OutFailure, PropertyFailure(TEXT("fixed_array_length_mismatch"), TEXT("The fixed array requires exactly its declared number of elements."), PropertyPath, Object->TryGetField(TEXT("items")), MakeShared<FJsonValueNumber>(Property->ArrayDim), TEXT("/items")), FString());
      return false;
    }
    TArray<TSharedPtr<FJsonValue>> CompletedItems;
    for (int32 Index = 0; Index < Items->Num(); ++Index) {
      const bool bCompleted = [&]() -> bool {
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
      {
        UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(OutFailure.Diagnostic, FString::Printf(TEXT("/items/%d"), Index));
        return false;
      }
      CompletedItems.Add(Completed);
      OutNode.Children.Add(MoveTemp(Child));
    
        return true;
      }();
      if (!bCompleted) {
        if (!Session.bCollectValueDiagnostics) return false;
        ChildFailures.Append(OutFailure.Diagnostic);
        OutFailure = FFailure();
      }
    }
    if (!ChildFailures.Code.IsEmpty()) {
      SetFailure(OutFailure, ChildFailures, FString(), PropertyPath);
      return false;
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
        !UeAgentAuthoringReflection::IsOwnedObjectProperty(Property)
        && !UeAgentAuthoringReflection::IsExistingInlineObjectProperty(
            Property, BaseValuePtr, Owner)) {
      SetFailure(OutFailure, PropertyFailure(TEXT("external_reference_recursive_patch_forbidden"), TEXT("Recursive property edits cannot follow a reference outside the requested owner."), PropertyPath), FString());
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
        SetFailure(OutFailure, PropertyFailure(TEXT("missing_struct_fields"), TEXT("The struct edit requires a fields object."), PropertyPath, InputObject->TryGetField(TEXT("fields")), MakeShared<FJsonValueString>(TEXT("object")), TEXT("/fields")), FString());
        return false;
      }

      TSharedPtr<FPropertyValueStorage> DefaultStorage;
      if (!BaseValuePtr) {
        FDiagnostic StorageError;
        DefaultStorage =
            FPropertyValueStorage::CreateInitialized(Property, StorageError);
        if (!DefaultStorage.IsValid()) {
          SetFailure(OutFailure, StorageError, FString(), PropertyPath);
          return false;
        }
        BaseValuePtr = DefaultStorage->GetValuePtr();
      }
      TSharedPtr<FJsonValue> BaseEncoded = UeAgentPropertyValueCodec::Encode(
          Property, BaseValuePtr, Owner, MakeCodecOptions(Options), 0, nullptr, bIgnoreFixedArray);
      TSharedPtr<FJsonObject> CompletedObject =
          BaseEncoded.IsValid() && BaseEncoded->Type == EJson::Object
              ? BaseEncoded->AsObject()
              : nullptr;
      const TSharedPtr<FJsonObject> *CompletedFieldsPtr = nullptr;
      if (!CompletedObject.IsValid() ||
          !CompletedObject->TryGetObjectField(TEXT("fields"),
                                              CompletedFieldsPtr) ||
          !CompletedFieldsPtr || !CompletedFieldsPtr->IsValid()) {
        auto Diagnostic = PropertyFailure(TEXT("struct_base_encode_failed"), TEXT("The current struct value could not be encoded with a fields object for preserving untouched members."), PropertyPath);
        Diagnostic.SourceKind = TEXT("native_result");
        SetFailure(OutFailure, MoveTemp(Diagnostic));
        return false;
      }
      TSharedPtr<FJsonObject> CompletedFields = *CompletedFieldsPtr;
      TArray<FString> FieldNames;
      (*InputFields)->Values.GetKeys(FieldNames);
      FieldNames.Sort();
      for (const FString &FieldName : FieldNames) {
        const bool bCompleted = [&]() -> bool {
        FProperty *Field =
            FindFProperty<FProperty>(StructProperty->Struct, *FieldName);
        FDiagnostic FieldReason;
        if (!Field || !IsEditableForApply(Field, Owner, Options, FieldReason, StructProperty->Struct)) {
          SetFailure(OutFailure,
                     Field ? FieldReason : PropertyFailure(TEXT("unknown_struct_field"), TEXT("The requested field does not exist in the native struct."), PropertyPath + TEXT(".") + FieldName, MakeShared<FJsonValueString>(FieldName), MakeShared<FJsonValueString>(StructProperty->Struct->GetPathName()), TEXT("/fields/") + FieldName.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"))),
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
        {
          UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(OutFailure.Diagnostic,
              TEXT("/fields/") + FieldName.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")));
          return false;
        }
        CompletedFields->SetField(FieldName, CompletedField);
        OutNode.Children.Add(MoveTemp(Child));
      
          return true;
        }();
        if (!bCompleted) {
          if (!Session.bCollectValueDiagnostics) return false;
          ChildFailures.Append(OutFailure.Diagnostic);
          OutFailure = FFailure();
        }
      }
      if (!ChildFailures.Code.IsEmpty()) {
        SetFailure(OutFailure, ChildFailures, FString(), PropertyPath);
        return false;
      }
      OutCompletedValue =
          MakeShared<FJsonValueObject>(CompletedObject.ToSharedRef());
      return CaptureNativeStructBaseline(Property, BaseValuePtr, OutCompletedValue,
          bIgnoreFixedArray, Entry, PropertyPath, OutFailure);
    }
    if (StructKind == TEXT("property_bag")) {
      FInstancedPropertyBag SchemaBag;
      if (BaseValuePtr) SchemaBag = *static_cast<const FInstancedPropertyBag*>(BaseValuePtr);
      FDiagnostic SchemaError;
      if (!UeAgentPropertyValueCodec::PreparePropertyBagSchema(InputValue, SchemaBag,
              MakeCodecOptions(Options), SchemaError)) {
        SetFailure(OutFailure, SchemaError, FString(), PropertyPath);
        return false;
      }
      const auto InputFields = InputValue->AsObject()->GetObjectField(TEXT("fields"));
      const auto CompletedFields = MakeShared<FJsonObject>();
      if (const auto* BagType = SchemaBag.GetPropertyBagStruct()) {
        // Deferred text edits retain native field pointers until apply/rollback.
        Entry.DynamicFieldTypes.Emplace(const_cast<UPropertyBag*>(BagType));
        for (const auto& Desc : BagType->GetPropertyDescs()) {
          const bool bCompleted = [&]() -> bool {
          FProperty* Field = const_cast<FProperty*>(Desc.CachedProperty);
          FDiagnostic FieldReason;
          if (!Field || !IsEditableForApply(Field, Owner, Options, FieldReason, BagType)) {
            SetFailure(OutFailure, Field ? FieldReason : PropertyFailure(TEXT("property_bag_field_unavailable"), TEXT("The property bag field has no reflected native property."), PropertyPath + TEXT(".") + Desc.Name.ToString(), MakeShared<FJsonValueString>(Desc.Name.ToString())),
                Desc.Name.ToString(), PropertyPath + TEXT(".") + Desc.Name.ToString());
            return false;
          }
          TSharedPtr<FJsonValue> CompletedField;
          FResolvedNode Child;
          if (!BuildCompletedValue(Field, InputFields->TryGetField(Desc.Name.ToString()),
                  Field->ContainerPtrToValuePtr<void>(SchemaBag.GetValue().GetMemory()), Owner,
                  Prepared, Entry, Options, false, bHashKeyContext, Session,
                  PropertyPath + TEXT(".") + Desc.Name.ToString(), CompletedField, Child, OutFailure))
          {
            UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(OutFailure.Diagnostic,
                TEXT("/fields/") + Desc.Name.ToString().Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")));
            return false;
          }
          CompletedFields->SetField(Desc.Name.ToString(), CompletedField);
          OutNode.Children.Add(MoveTemp(Child));
        
            return true;
          }();
          if (!bCompleted) {
            if (!Session.bCollectValueDiagnostics) return false;
            ChildFailures.Append(OutFailure.Diagnostic);
            OutFailure = FFailure();
          }
        }
        if (!ChildFailures.Code.IsEmpty()) {
          SetFailure(OutFailure, ChildFailures, FString(), PropertyPath);
          return false;
        }
      }
      const auto Completed = MakeShared<FJsonObject>();
      Completed->SetField(TEXT("schema"), InputValue->AsObject()->TryGetField(TEXT("schema")));
      Completed->SetObjectField(TEXT("fields"), CompletedFields);
      OutCompletedValue = MakeShared<FJsonValueObject>(Completed);
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
      if (!DynamicStruct) {
        SetFailure(OutFailure, PropertyFailure(TEXT("instanced_struct_type_or_fields_invalid"),
            TEXT("The requested instanced struct type could not be loaded."), PropertyPath,
            MakeShared<FJsonValueString>(StructPath), MakeShared<FJsonValueString>(TEXT("loadable struct type")), TEXT("/type_path")));
        return false;
      }
      if (!InputObject->TryGetObjectField(TEXT("fields"), InputFields) || !InputFields || !InputFields->IsValid()) {
        SetFailure(OutFailure, PropertyFailure(TEXT("instanced_struct_type_or_fields_invalid"),
            TEXT("The instanced struct edit requires a fields object."), PropertyPath,
            InputObject->TryGetField(TEXT("fields")), MakeShared<FJsonValueString>(TEXT("object")), TEXT("/fields")));
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
      // Complete the edit from the same captured shape the codec consumes.
      // Noneditable members remain in the baseline, not in the authored patch.
      const FInstancedStruct* ValueBase =
          Existing && Existing->GetScriptStruct() == DynamicStruct ? Existing : &DefaultDynamic;
      const auto EncodedBase = UeAgentPropertyValueCodec::Encode(
          Property, ValueBase, Owner, MakeCodecOptions(Options), 0, nullptr, bIgnoreFixedArray);
      const auto CompletedObject = EncodedBase && EncodedBase->Type == EJson::Object
          ? EncodedBase->AsObject() : nullptr;
      const TSharedPtr<FJsonObject>* CompletedFieldsPtr = nullptr;
      if (!CompletedObject || !CompletedObject->TryGetObjectField(TEXT("fields"), CompletedFieldsPtr)
          || !CompletedFieldsPtr || !CompletedFieldsPtr->IsValid()) {
        SetFailure(OutFailure, PropertyFailure(TEXT("instanced_struct_base_encode_failed"), TEXT("The current instanced struct could not be encoded with a fields object."), PropertyPath, MakeShared<FJsonValueString>(StructPath), MakeShared<FJsonValueString>(TEXT("encodable native struct"))), StructPath);
        return false;
      }
      const auto CompletedFields = *CompletedFieldsPtr;
      TArray<FString> FieldNames;
      (*InputFields)->Values.GetKeys(FieldNames);
      FieldNames.Sort();
      for (const FString& FieldName : FieldNames) {
        const bool bCompleted = [&]() -> bool {
        FProperty* Field = FindFProperty<FProperty>(DynamicStruct, *FieldName);
        if (!Field) {
          SetFailure(OutFailure, PropertyFailure(TEXT("unknown_instanced_struct_field"), TEXT("The requested field does not exist in the selected instanced struct type."), PropertyPath + TEXT(".") + FieldName, MakeShared<FJsonValueString>(FieldName), MakeShared<FJsonValueString>(DynamicStruct->GetPathName()), TEXT("/fields/") + FieldName.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"))), FieldName);
          return false;
        }
        const void* FieldBase = Field->ContainerPtrToValuePtr<void>(DynamicBase);
        const auto ExplicitField = (*InputFields)->TryGetField(FieldName);
        FDiagnostic FieldReason;
        if (!IsEditableForApply(Field, Owner, Options, FieldReason, DynamicStruct)) {
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
        {
          UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(OutFailure.Diagnostic,
              TEXT("/fields/") + FieldName.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")));
          return false;
        }
        CompletedFields->SetField(Field->GetName(), CompletedField);
        OutNode.Children.Add(MoveTemp(Child));
      
          return true;
        }();
        if (!bCompleted) {
          if (!Session.bCollectValueDiagnostics) return false;
          ChildFailures.Append(OutFailure.Diagnostic);
          OutFailure = FFailure();
        }
      }
      if (!ChildFailures.Code.IsEmpty()) {
        SetFailure(OutFailure, ChildFailures, FString(), PropertyPath);
        return false;
      }
      OutCompletedValue = MakeShared<FJsonValueObject>(CompletedObject);
      return CaptureNativeStructBaseline(Property, ValueBase, OutCompletedValue,
          bIgnoreFixedArray, Entry, PropertyPath, OutFailure);
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
    if (!InputObject->TryGetArrayField(TEXT("items"), Items) || !Items) {
      SetFailure(OutFailure, PropertyFailure(TEXT("collection_items_invalid"), TEXT("The collection edit requires an items array."), PropertyPath, InputObject->TryGetField(TEXT("items")), MakeShared<FJsonValueString>(TEXT("array")), TEXT("/items")), FString()); return false;
    }
    TUniquePtr<FScriptArrayHelper> BaseHelper;
    if (BaseValuePtr)
      BaseHelper = MakeUnique<FScriptArrayHelper>(ArrayProperty, BaseValuePtr);
    if (ArrayProperty->HasAnyPropertyFlags(CPF_EditFixedSize)
        && Items->Num() != (BaseHelper ? BaseHelper->Num() : 0)
        && !(Options.AuthorizeArrayResize && Options.AuthorizeArrayResize(ArrayProperty))) {
      SetFailure(OutFailure, PropertyFailure(TEXT("edit_fixed_size_array_length_mismatch"), TEXT("This array is fixed size and the edit changes its element count."), PropertyPath, MakeShared<FJsonValueNumber>(Items->Num()), MakeShared<FJsonValueNumber>(BaseHelper ? BaseHelper->Num() : 0), TEXT("/items")), Property->GetName());
      return false;
    }
    TArray<TSharedPtr<FJsonValue>> CompletedItems;
    OutNode.CapturedArrayCount = BaseHelper ? BaseHelper->Num() : 0;
    for (int32 Index = 0; Index < Items->Num(); ++Index) {
      const bool bCompleted = [&]() -> bool {
      int32 CapturedIndex = BaseHelper && Index < BaseHelper->Num() ? Index : INDEX_NONE;
      TSharedPtr<FPropertyValueStorage> DefaultValue;
      const void *ElementBase =
          BaseHelper.IsValid() && Index < BaseHelper->Num()
              ? BaseHelper->GetRawPtr(Index)
              : nullptr;
      TSharedPtr<FJsonValue> ElementInput = (*Items)[Index];
      if (CastField<FStructProperty>(ArrayProperty->Inner) && ElementInput->Type == EJson::Object
          && ElementInput->AsObject()->HasField(TEXT("from"))) {
        const auto Object = ElementInput->AsObject();
        const auto Source = Object->TryGetField(TEXT("from"));
        ElementBase = nullptr;
        CapturedIndex = INDEX_NONE;
        if (Source.IsValid() && Source->Type != EJson::Null) {
          int32 SourceIndex = INDEX_NONE;
          FDiagnostic SourceError;
          if (!UeAgentAuthoringJsonFields::ReadInt32Field(*Object, TEXT("from"), SourceIndex, SourceError)) {
            SetFailure(OutFailure, SourceError, FString(), PropertyPath); return false;
          }
          if (!BaseHelper || SourceIndex < 0 || SourceIndex >= BaseHelper->Num()) {
            SetFailure(OutFailure, PropertyFailure(TEXT("array_source_not_found"), TEXT("The from index is outside the captured array."), PropertyPath, MakeShared<FJsonValueNumber>(SourceIndex), MakeShared<FJsonValueString>(FString::Printf(TEXT("index in [0, %d)"), BaseHelper ? BaseHelper->Num() : 0)), FString::Printf(TEXT("/items/%d/from"), Index)), FString()); return false;
          }
          ElementBase = BaseHelper->GetRawPtr(SourceIndex);
          CapturedIndex = SourceIndex;
        }
        auto Desired = MakeShared<FJsonObject>();
        Desired->Values = Object->Values;
        Desired->RemoveField(TEXT("from"));
        ElementInput = MakeShared<FJsonValueObject>(Desired);
      }
      // The enclosing structured item may itself have moved. Its destination
      // array is not the captured source container, so resolve the restored
      // exact binding in the native owner rather than at any destination index.
      if (auto* ObjectProperty = CastField<FObjectPropertyBase>(ArrayProperty->Inner);
          ObjectProperty && UeAgentAuthoringReflection::IsOwnedObjectProperty(ObjectProperty)
          && (*Items)[Index]->Type == EJson::Object) {
        const TSharedPtr<FJsonObject>* Owned = nullptr;
        if ((*Items)[Index]->AsObject()->TryGetObjectField(TEXT("owned_object"), Owned)
            && (*Owned)->HasField(TEXT("owner_relative_path"))) {
          FString RelativePath;
          if (!(*Owned)->TryGetStringField(TEXT("owner_relative_path"), RelativePath)) {
            SetFailure(OutFailure, PropertyFailure(TEXT("owned_object_binding_invalid"), TEXT("The owned object binding must be a string relative to its owner."), PropertyPath), FString()); return false;
          }
          ElementBase = nullptr;
          if (!RelativePath.IsEmpty()) {
            UObject* CapturedObject = Owner ? StaticFindObject(UObject::StaticClass(), Owner, *RelativePath) : nullptr;
            if (!CapturedObject || !IsObjectWithin(CapturedObject, Owner)
                || CapturedObject->GetPathName(Owner) != RelativePath
                || !CapturedObject->IsA(ObjectProperty->PropertyClass)) {
              SetFailure(OutFailure, PropertyFailure(TEXT("owned_object_binding_not_found"), TEXT("The requested owned object binding does not identify an object of the required class within this owner."), PropertyPath), FString()); return false;
            }
            FDiagnostic StorageError;
            DefaultValue = FPropertyValueStorage::CreateInitialized(ArrayProperty->Inner, StorageError);
            if (!DefaultValue.IsValid()) {
              SetFailure(OutFailure, StorageError, FString(), PropertyPath); return false;
            }
            ObjectProperty->SetObjectPropertyValue(DefaultValue->GetMutableValuePtr(), CapturedObject);
            ElementBase = DefaultValue->GetValuePtr();
          }
        }
      }
      if (!ElementBase) {
        FDiagnostic StorageError;
        DefaultValue = FPropertyValueStorage::CreateInitialized(
            ArrayProperty->Inner, StorageError);
        if (!DefaultValue.IsValid()) {
          SetFailure(OutFailure, StorageError, FString(), PropertyPath);
          return false;
        }
        ElementBase = DefaultValue->GetValuePtr();
      }
      TSharedPtr<FJsonValue> Completed;
      FResolvedNode Child;
      TGuardValue<FArrayProperty*> OwnedArrayGuard(Session.OwnedArray, ArrayProperty);
	  TGuardValue<FString> OwnedArrayPathGuard(Session.OwnedArrayPath, PropertyPath);
      TGuardValue<int32> OwnedIndexGuard(Session.OwnedIndex, Index);
      if (!BuildCompletedValue(
              ArrayProperty->Inner, ElementInput, ElementBase, Owner,
              Prepared, Entry, Options, false, false, Session,
              FString::Printf(TEXT("%s[%d]"), *PropertyPath, Index), Completed,
              Child, OutFailure))
      {
        UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(OutFailure.Diagnostic, FString::Printf(TEXT("/items/%d"), Index));
        return false;
      }
      CompletedItems.Add(Completed);
      OutNode.CapturedArrayIndices.Add(CapturedIndex);
      OutNode.Children.Add(MoveTemp(Child));
    
        return true;
      }();
      if (!bCompleted) {
        if (!Session.bCollectValueDiagnostics) return false;
        ChildFailures.Append(OutFailure.Diagnostic);
        OutFailure = FFailure();
      }
    }
    if (!ChildFailures.Code.IsEmpty()) {
      SetFailure(OutFailure, ChildFailures, FString(), PropertyPath);
      return false;
    }
    TSharedRef<FJsonObject> CompletedObject = MakeShared<FJsonObject>();
    CompletedObject->SetArrayField(TEXT("items"), CompletedItems);
    OutCompletedValue = MakeShared<FJsonValueObject>(CompletedObject);
    return true;
  }

  if (FSetProperty *SetProperty = CastField<FSetProperty>(Property)) {
    const bool bRequiresSource = UeAgentPropertyValueCodec::RequiresHashSource(SetProperty->ElementProp);
    TArray<const void*> Candidates;
    if (BaseValuePtr) {
      FScriptSetHelper BaseSet(SetProperty, BaseValuePtr);
      for (int32 BaseIndex = 0; BaseIndex < BaseSet.GetMaxIndex(); ++BaseIndex)
        if (BaseSet.IsValidIndex(BaseIndex)) Candidates.Add(BaseSet.GetElementPtr(BaseIndex));
    }
    TSharedPtr<FJsonObject> InputObject;
    if (!GetStrictObject(InputValue, InputObject, TEXT("set_value_invalid"),
                         OutFailure, PropertyPath) ||
        !HasOnlyFields(InputObject, {TEXT("items")}, OutFailure, PropertyPath,
                       TEXT("set_unknown_field")))
      return false;
    const TArray<TSharedPtr<FJsonValue>> *Items = nullptr;
    if (!InputObject->TryGetArrayField(TEXT("items"), Items) || !Items) {
      SetFailure(OutFailure, PropertyFailure(TEXT("collection_items_invalid"), TEXT("The collection edit requires an items array."), PropertyPath, InputObject->TryGetField(TEXT("items")), MakeShared<FJsonValueString>(TEXT("array")), TEXT("/items")), FString()); return false;
    }
    TArray<TSharedPtr<FJsonValue>> CompletedItems;
    for (int32 Index = 0; Index < Items->Num(); ++Index) {
      const bool bCompleted = [&]() -> bool {
      FDiagnostic StorageError;
      TSharedPtr<FPropertyValueStorage> DefaultValue =
          FPropertyValueStorage::CreateInitialized(SetProperty->ElementProp,
                                                   StorageError);
      if (!DefaultValue.IsValid()) {
        SetFailure(OutFailure, StorageError, FString(), PropertyPath);
        return false;
      }
      TSharedPtr<FJsonValue> ElementInput = (*Items)[Index];
      const void* ElementBase = DefaultValue->GetValuePtr();
      int32 SourceIndex = INDEX_NONE;
      if (bRequiresSource) {
        const auto ElementObject = ElementInput && ElementInput->Type == EJson::Object ? ElementInput->AsObject() : nullptr;
        if (!ResolveHashSource(SetProperty->ElementProp, ElementObject ? ElementObject->TryGetField(TEXT("from")) : nullptr,
            Candidates, Owner, Options, PropertyPath, SourceIndex, OutFailure)) return false;
        auto Desired = MakeShared<FJsonObject>(); Desired->Values = ElementObject->Values; Desired->RemoveField(TEXT("from"));
        ElementInput = MakeShared<FJsonValueObject>(Desired);
        if (SourceIndex != INDEX_NONE) ElementBase = Candidates[SourceIndex];
      } else {
      // Complete types retain the existing value-key matching contract.
      // Probe edits are isolated: deferred payload edits must only enter the
      // final preparation once, after selecting the matching native base.
      FPreparedProperty ProbeEntry;
      FResolvedNode ProbeNode;
      TSharedPtr<FJsonValue> ProbeValue;
      if (!BuildCompletedValue(SetProperty->ElementProp, (*Items)[Index],
              DefaultValue->GetValuePtr(), Owner, Prepared, ProbeEntry, Options,
              false, true, Session, FString::Printf(TEXT("%s{%d}"), *PropertyPath, Index),
              ProbeValue, ProbeNode, OutFailure)) {
        UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(OutFailure.Diagnostic, FString::Printf(TEXT("/items/%d"), Index));
        return false;
      }
      TSharedPtr<FPropertyValueStorage> IdentityValue =
          FPropertyValueStorage::CreateInitialized(SetProperty->ElementProp, StorageError);
      if (!IdentityValue.IsValid() || !UeAgentPropertyValueCodec::Decode(
              SetProperty->ElementProp, ProbeValue, IdentityValue->GetMutableValuePtr(),
              Owner, MakeCodecOptions(Options), StorageError)) {
        SetFailure(OutFailure, StorageError, FString(), PropertyPath);
        return false;
      }
      if (BaseValuePtr) {
        FScriptSetHelper BaseSet(SetProperty, BaseValuePtr);
        for (int32 BaseIndex = 0; BaseIndex < BaseSet.GetMaxIndex(); ++BaseIndex) {
          if (BaseSet.IsValidIndex(BaseIndex) && SetProperty->ElementProp->Identical(
                  IdentityValue->GetValuePtr(), BaseSet.GetElementPtr(BaseIndex), PPF_None)) {
            ElementBase = BaseSet.GetElementPtr(BaseIndex);
            break;
          }
        }
      }
      }
      TSharedPtr<FJsonValue> Completed;
      FResolvedNode Child;
      if (!BuildCompletedValue(
              SetProperty->ElementProp, ElementInput,
              ElementBase, Owner, Prepared, Entry, Options,
              false, true, Session,
              FString::Printf(TEXT("%s{%d}"), *PropertyPath, Index), Completed,
              Child, OutFailure))
      {
        UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(OutFailure.Diagnostic, FString::Printf(TEXT("/items/%d"), Index));
        return false;
      }
      if (bRequiresSource && !ValidateCompletedHashIdentity(SetProperty->ElementProp, Completed,
          Candidates, SourceIndex, Owner, Options, PropertyPath, OutFailure)) return false;
      CompletedItems.Add(Completed);
      OutNode.Children.Add(MoveTemp(Child));
    
        return true;
      }();
      if (!bCompleted) {
        if (!Session.bCollectValueDiagnostics) return false;
        ChildFailures.Append(OutFailure.Diagnostic);
        OutFailure = FFailure();
      }
    }
    if (!ChildFailures.Code.IsEmpty()) {
      SetFailure(OutFailure, ChildFailures, FString(), PropertyPath);
      return false;
    }
    TSharedRef<FJsonObject> CompletedObject = MakeShared<FJsonObject>();
    CompletedObject->SetArrayField(TEXT("items"), CompletedItems);
    OutCompletedValue = MakeShared<FJsonValueObject>(CompletedObject);
    return true;
  }

  if (FMapProperty *MapProperty = CastField<FMapProperty>(Property)) {
    const bool bRequiresSource = UeAgentPropertyValueCodec::RequiresHashSource(MapProperty->KeyProp);
    TArray<const void*> Candidates, CandidateValues;
    if (BaseValuePtr) {
      FScriptMapHelper BaseMap(MapProperty, BaseValuePtr);
      for (int32 BaseIndex = 0; BaseIndex < BaseMap.GetMaxIndex(); ++BaseIndex)
        if (BaseMap.IsValidIndex(BaseIndex)) {
          Candidates.Add(BaseMap.GetKeyPtr(BaseIndex)); CandidateValues.Add(BaseMap.GetValuePtr(BaseIndex));
        }
    }
    TSharedPtr<FJsonObject> InputObject;
    if (!GetStrictObject(InputValue, InputObject, TEXT("map_value_invalid"),
                         OutFailure, PropertyPath) ||
        !HasOnlyFields(InputObject, {TEXT("entries")}, OutFailure, PropertyPath,
                       TEXT("map_unknown_field")))
      return false;
    const TArray<TSharedPtr<FJsonValue>> *Entries = nullptr;
    if (!InputObject->TryGetArrayField(TEXT("entries"), Entries) || !Entries) {
      SetFailure(OutFailure, PropertyFailure(TEXT("map_entries_invalid"), TEXT("The map edit requires an entries array."), PropertyPath, InputObject->TryGetField(TEXT("entries")), MakeShared<FJsonValueString>(TEXT("array")), TEXT("/entries")), FString()); return false;
    }
    TArray<TSharedPtr<FJsonValue>> CompletedEntries;
    for (int32 Index = 0; Index < Entries->Num(); ++Index) {
      bool bMappedValueChecked = false;
      const bool bCompleted = [&]() -> bool {
      TSharedPtr<FJsonObject> EntryObject;
      if (!GetStrictObject((*Entries)[Index], EntryObject,
                           TEXT("map_entry_invalid"), OutFailure,
                           PropertyPath) ||
          !HasOnlyFields(EntryObject, bRequiresSource ? std::initializer_list<const TCHAR*>{TEXT("key"), TEXT("value"), TEXT("from")}
              : std::initializer_list<const TCHAR*>{TEXT("key"), TEXT("value")}, OutFailure,
                         PropertyPath, TEXT("map_entry_unknown_field")))
        return false;
      const TSharedPtr<FJsonValue> KeyInput =
          EntryObject->TryGetField(TEXT("key"));
      const TSharedPtr<FJsonValue> MappedInput =
          EntryObject->TryGetField(TEXT("value"));
      if (!KeyInput.IsValid() || !MappedInput.IsValid()) {
        SetFailure(OutFailure, PropertyFailure(TEXT("map_entry_key_or_value_missing"), TEXT("Each map entry must provide both key and value."), PropertyPath, nullptr, MakeShared<FJsonValueString>(TEXT("key and value fields")), FString::Printf(TEXT("/entries/%d/%s"), Index, KeyInput.IsValid() ? TEXT("value") : TEXT("key"))), LexToString(Index));
        return false;
      }
      FDiagnostic StorageError;
      TSharedPtr<FPropertyValueStorage> KeyDefault =
          FPropertyValueStorage::CreateInitialized(MapProperty->KeyProp,
                                                   StorageError);
      if (!KeyDefault.IsValid()) {
        SetFailure(OutFailure, StorageError, FString(), FString::Printf(TEXT("%s[%d].key"), *PropertyPath, Index));
        return false;
      }
      TSharedPtr<FPropertyValueStorage> ValueDefault =
          FPropertyValueStorage::CreateInitialized(MapProperty->ValueProp,
                                                   StorageError);
      if (!ValueDefault.IsValid()) {
        SetFailure(OutFailure, StorageError, FString(), PropertyPath);
        return false;
      }
      TSharedPtr<FJsonValue> CompletedKey;
      TSharedPtr<FJsonValue> CompletedMapped;
      FResolvedNode KeyNode;
      FResolvedNode ValueNode;
      int32 SourceIndex = INDEX_NONE;
      if (bRequiresSource && !ResolveHashSource(MapProperty->KeyProp, EntryObject->TryGetField(TEXT("from")),
          Candidates, Owner, Options, PropertyPath, SourceIndex, OutFailure)) return false;
      const void* KeyBase = SourceIndex == INDEX_NONE ? KeyDefault->GetValuePtr() : Candidates[SourceIndex];
      if (!BuildCompletedValue(
              MapProperty->KeyProp, KeyInput, KeyBase, Owner,
              Prepared, Entry, Options, false, true, Session,
              FString::Printf(TEXT("%s[%d].key"), *PropertyPath, Index),
              CompletedKey, KeyNode, OutFailure))
      {
        UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(OutFailure.Diagnostic, FString::Printf(TEXT("/entries/%d/key"), Index));
        return false;
      }
      if (bRequiresSource && !ValidateCompletedHashIdentity(MapProperty->KeyProp, CompletedKey,
          Candidates, SourceIndex, Owner, Options, PropertyPath, OutFailure)) return false;
      // Match the completed native key, not the proposed entry position. Sparse
      // edits inherit the matching value; only new keys use native defaults.
      if (!UeAgentPropertyValueCodec::Decode(MapProperty->KeyProp, CompletedKey,
              KeyDefault->GetMutableValuePtr(), Owner, MakeCodecOptions(Options), StorageError)) {
        SetFailure(OutFailure, StorageError, FString(), FString::Printf(TEXT("%s[%d].key"), *PropertyPath, Index));
        return false;
      }
      const void* MappedBase = SourceIndex == INDEX_NONE ? ValueDefault->GetValuePtr() : CandidateValues[SourceIndex];
      if (!bRequiresSource && BaseValuePtr) {
        FScriptMapHelper BaseMap(MapProperty, BaseValuePtr);
        for (int32 BaseIndex = 0; BaseIndex < BaseMap.GetMaxIndex(); ++BaseIndex) {
          if (BaseMap.IsValidIndex(BaseIndex) && MapProperty->KeyProp->Identical(
                  KeyDefault->GetValuePtr(), BaseMap.GetKeyPtr(BaseIndex))) {
            MappedBase = BaseMap.GetValuePtr(BaseIndex);
            break;
          }
        }
      }
      bMappedValueChecked = true;
      if (!BuildCompletedValue(
              MapProperty->ValueProp, MappedInput, MappedBase,
              Owner, Prepared, Entry, Options, false, false, Session,
              FString::Printf(TEXT("%s[%d].value"), *PropertyPath, Index),
              CompletedMapped, ValueNode, OutFailure))
      {
        UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(OutFailure.Diagnostic, FString::Printf(TEXT("/entries/%d/value"), Index));
        return false;
      }
      TSharedRef<FJsonObject> CompletedEntry = MakeShared<FJsonObject>();
      CompletedEntry->SetField(TEXT("key"), CompletedKey);
      CompletedEntry->SetField(TEXT("value"), CompletedMapped);
      CompletedEntries.Add(MakeShared<FJsonValueObject>(CompletedEntry));
      OutNode.Children.Add(MoveTemp(KeyNode));
      OutNode.Children.Add(MoveTemp(ValueNode));
    
        return true;
      }();
      if (!bCompleted) {
        if (!Session.bCollectValueDiagnostics) return false;
        if (!bMappedValueChecked) {
          const auto Skipped = MakeShared<FJsonObject>();
          Skipped->SetStringField(TEXT("stage"), TEXT("map_value_prepare"));
          Skipped->SetStringField(TEXT("property_path"), FString::Printf(TEXT("%s[%d].value"), *PropertyPath, Index));
          Skipped->SetStringField(TEXT("reason"), TEXT("map_key_or_source_invalid"));
          OutFailure.Diagnostic.Unchecked.Add(MakeShared<FJsonValueObject>(Skipped));
        }
        ChildFailures.Append(OutFailure.Diagnostic);
        OutFailure = FFailure();
      }
    }
    if (!ChildFailures.Code.IsEmpty()) {
      SetFailure(OutFailure, ChildFailures, FString(), PropertyPath);
      return false;
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
      SetFailure(OutFailure, PropertyFailure(TEXT("optional_is_set_missing"), TEXT("An optional value requires a boolean is_set field."), PropertyPath, InputObject->TryGetField(TEXT("is_set")), MakeShared<FJsonValueString>(TEXT("boolean")), TEXT("/is_set")), FString());
      return false;
    }
    if (!bIsSet) {
      if (InputObject->Values.Num() != 1) {
        SetFailure(OutFailure, PropertyFailure(TEXT("optional_unset_has_value"), TEXT("An unset optional must not contain a value."), PropertyPath, InputObject->TryGetField(TEXT("value")), MakeShared<FJsonValueString>(TEXT("no value when is_set is false")), TEXT("/value")), FString());
        return false;
      }
      OutCompletedValue = InputValue;
      return true;
    }
    const TSharedPtr<FJsonValue> InnerInput =
        InputObject->TryGetField(TEXT("value"));
    if (!InnerInput.IsValid()) {
      SetFailure(OutFailure, PropertyFailure(TEXT("optional_value_missing"), TEXT("A set optional must provide its value."), PropertyPath, nullptr, MakeShared<FJsonValueString>(TEXT("value when is_set is true")), TEXT("/value")), FString());
      return false;
    }
    FDiagnostic StorageError;
    TSharedPtr<FPropertyValueStorage> InnerDefault;
    const void *InnerBase =
        BaseValuePtr && OptionalProperty->IsSet(BaseValuePtr)
            ? OptionalProperty->GetValuePointerForRead(BaseValuePtr)
            : nullptr;
    if (!InnerBase) {
      InnerDefault = FPropertyValueStorage::CreateInitialized(
          OptionalProperty->GetValueProperty(), StorageError);
      if (!InnerDefault.IsValid()) {
        SetFailure(OutFailure, StorageError, FString(), PropertyPath);
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
    {
      UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(OutFailure.Diagnostic, TEXT("/value"));
      return false;
    }
    TSharedRef<FJsonObject> CompletedObject = MakeShared<FJsonObject>();
    CompletedObject->SetBoolField(TEXT("is_set"), true);
    CompletedObject->SetField(TEXT("value"), CompletedInner);
    OutNode.Children.Add(MoveTemp(Child));
    OutCompletedValue = MakeShared<FJsonValueObject>(CompletedObject);
    return true;
  }

  if (Session.bCollectValueDiagnostics) {
    FDiagnostic Error;
    auto Scratch = FPropertyValueStorage::CreateInitialized(Property, Error);
    if (!Scratch.IsValid() || !UeAgentPropertyValueCodec::Decode(Property, InputValue,
        Scratch->GetMutableValuePtr(), Owner, MakeCodecOptions(Options), Error, 0, bIgnoreFixedArray)) {
      SetFailure(OutFailure, Error, FString(), PropertyPath);
      if (!OutFailure.Diagnostic.ExpectedValue.IsValid())
        OutFailure.Diagnostic.ExpectedValue = MakeShared<FJsonValueObject>(
            UeAgentPropertyValueCodec::BuildTypeDescriptor(Property, bIgnoreFixedArray));
      if (!OutFailure.Diagnostic.ActualValue.IsValid() && !OutFailure.Diagnostic.ActualValuePresent.IsSet()) {
        OutFailure.Diagnostic.ActualValue = RawInputValue;
        OutFailure.Diagnostic.ActualValuePresent = RawInputValue.IsValid();
      }
      return false;
    }
  }
  OutCompletedValue = InputValue;
  return true;
}

bool ReplaceScratchReferences(FProperty *Property, void *ValuePtr,
                              const TArray<FMaterialization> &Materializations,
                              bool bIgnoreFixedArray, FDiagnostic& OutError) {
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
                             bool bIgnoreFixedArray, FDiagnostic& OutError) {
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
    OutError = FDiagnostic::Failure(TEXT("owned_object_reference_outside_owner"),
        TEXT("The owned object reference points outside its owner."), TEXT("object_apply"));
    OutError.InternalContext = Object->GetPathName();
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

bool ValidateDescriptor(const TSharedPtr<FJsonValue> &DescriptorValue,
                        FString &OutPropertyPath,
                        TSharedPtr<FJsonValue> &OutValue,
                        FFailure &OutFailure) {
  const auto Descriptor = DescriptorValue.IsValid() && DescriptorValue->Type == EJson::Object
      ? DescriptorValue->AsObject() : nullptr;
  const auto FailDescriptor = [&](const FString& Code, const FString& Reason,
      const FString& Field, const TSharedPtr<FJsonValue>& Actual, const FString& Expected) {
    auto Diagnostic = FDiagnostic::Failure(Code, Reason, TEXT("object_apply_preflight"));
    Diagnostic.PropertyPath = OutPropertyPath;
    Diagnostic.Source.JsonPath = Field.IsEmpty() ? FString()
        : TEXT("/") + Field.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
    Diagnostic.CandidateName = Field;
    Diagnostic.ActualValue = Actual;
    Diagnostic.ActualValuePresent = Actual.IsValid();
    Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(Expected);
    Diagnostic.bRetrySafe = true;
    SetFailure(OutFailure, MoveTemp(Diagnostic), FString(), OutPropertyPath);
    return false;
  };
  if (!Descriptor.IsValid()) {
    return FailDescriptor(TEXT("invalid_property_descriptor"),
        TEXT("A property descriptor must be an object."), FString(), DescriptorValue, TEXT("object"));
  }
  Descriptor->TryGetStringField(TEXT("property_path"), OutPropertyPath);
  for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair :
       Descriptor->Values) {
    if (!PropertyDescriptorFields().Contains(Pair.Key)
        && !UeAgentAuthoringTypeDescriptors::IsReadOnlyTypeMetadataField(Pair.Key)) {
      return FailDescriptor(TEXT("unexpected_property_descriptor_field"),
          TEXT("The property descriptor contains an unsupported field."), Pair.Key,
          MakeShared<FJsonValueString>(Pair.Key), TEXT("supported property descriptor field"));
    }
  }
  if (!Descriptor->TryGetStringField(TEXT("property_path"), OutPropertyPath) ||
      OutPropertyPath.IsEmpty()) {
    return FailDescriptor(TEXT("missing_property_path"),
        TEXT("A property descriptor requires a non-empty property_path string."), TEXT("property_path"),
        Descriptor->TryGetField(TEXT("property_path")), TEXT("non-empty string"));
  }
  OutValue = Descriptor->TryGetField(TEXT("value"));
  if (!OutValue.IsValid()) {
    return FailDescriptor(TEXT("missing_value"),
        TEXT("A property descriptor requires a value field."), TEXT("value"), nullptr, TEXT("value"));
  }
  return true;
}

bool ValidateOptionalTypeAssertions(const FProperty *Property,
                                    const TSharedPtr<FJsonObject> &Descriptor,
                                    FDiagnostic& OutError) {
  TSharedPtr<FJsonObject> Completed =
      UeAgentPropertyValueCodec::BuildTypeDescriptor(Property);
  if (!Completed.IsValid()) {
    OutError = FDiagnostic::Failure(TEXT("invalid_type_descriptor"), TEXT("A native property and an object type descriptor are required."), TEXT("object_apply"));
    return false;
  }
  static const TSet<FString> AssertionFields = {
      TEXT("kind"),         TEXT("struct_path"),  TEXT("enum_path"),
      TEXT("element_type"), TEXT("key_type"),     TEXT("mapped_type"),
      TEXT("inner_type"),   TEXT("fixed_length"), TEXT("allowed_class"), TEXT("hash_source")};
  for (const FString &Field : AssertionFields) {
    if (const TSharedPtr<FJsonValue> Assertion = Descriptor->TryGetField(Field);
        Assertion.IsValid()) {
      Completed->SetField(Field, Assertion);
    }
  }
  return UeAgentPropertyValueCodec::ValidateTypeMetadata(Property, Completed,
                                                         OutError);
}

bool PrepareRootOnce(
    UStruct *RootStruct, void *RootData, UObject *RootObject,
    UObject *NotificationOwner, const FString &RootOwnerPath,
    const TArray<TSharedPtr<FJsonValue>> &PropertyDescriptors,
    const FOptions &RootOptions, FPrepareSession &Session,
    FPreparedApply &OutPrepared, FFailure &OutFailure) {
  // Root scope matches capture's root-field projection. Do not apply it again
  // to struct members or owned objects, even if they share the root's class.
  TOptional<FOptions> DescendantOptions;
  if (RootOptions.IncludeRootProperty) {
    DescendantOptions = RootOptions;
    DescendantOptions->IncludeRootProperty = {};
  }
  const FOptions& Options = DescendantOptions.IsSet() ? DescendantOptions.GetValue() : RootOptions;
  OutPrepared = FPreparedApply();
  OutFailure = FFailure();
  if (!RootStruct || !RootData || RootOwnerPath.IsEmpty()) {
    SetFailure(OutFailure, PropertyFailure(TEXT("object_apply_root_invalid"), TEXT("Property preparation requires a root type, live data, and an owner identity."), FString()), RootOwnerPath);
    return false;
  }
  if (RootObject) {
    FDiagnostic OwnerReason;
    if (!ValidateRootOwner(RootObject, Options, OwnerReason)) {
      SetFailure(OutFailure, OwnerReason, RootObject->GetPathName());
      return false;
    }
    if (Session.ObjectStack.Contains(RootObject)) {
      SetFailure(OutFailure, PropertyFailure(TEXT("object_identity_cycle"), TEXT("Recursive owned object editing revisited an object already being prepared."), FString()), RootObject->GetPathName());
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
  // Mesh callbacks validate material slots and animation against other fields on
  // the same component. They must see the complete patch, not descriptor order.
  // Keep each property notification (unlike a reconstruction's whole-object event).
  Prepared.bBatchNotifications = Options.bWholeObjectNotification
      || (NotificationOwner && NotificationOwner->IsA<UMeshComponent>());
  Prepared.bWholeObjectNotification = Options.bWholeObjectNotification;
  Prepared.bDeferTextSourceEdits = Session.bDeferTextSourceEdits;
  // Clone each occurrence, not each source pointer: callers may legitimately
  // reuse a JSON value at distinct native property/element positions.
  for (const auto& Descriptor : PropertyDescriptors)
    Prepared.SourceDescriptors.Add(Session.bUniqueDescriptorOccurrences ? Descriptor
        : UeAgentAuthoringState::CloneCanonicalValue(Descriptor));
  TGuardValue<bool> UniqueOccurrences(Session.bUniqueDescriptorOccurrences, true);

  FFailure FirstValueFailure;
  UeAgentAuthoringDiagnostics::FDiagnostic ValueDiagnostics;
  int32 DescriptorIndex = INDEX_NONE;
  auto AttachDescriptorSource = [&](const TSharedPtr<FJsonObject>& Descriptor) {
    if (Descriptor.IsValid()) {
      UeAgentAuthoringDiagnostics::LocatePropertyValueSource(OutFailure.Diagnostic, *Descriptor);
      if (Options.SupplementDiagnosticSource)
        Options.SupplementDiagnosticSource(OutFailure.Diagnostic, *Descriptor, RootStruct, RootData, DescriptorIndex);
    }
    if (OutFailure.Diagnostic.TargetPath.IsEmpty()) OutFailure.Diagnostic.TargetPath =
        Options.DiagnosticTargetPath.IsEmpty() ? RootOwnerPath : Options.DiagnosticTargetPath;
    else if (OutFailure.Diagnostic.TargetPath != RootOwnerPath && OutFailure.Diagnostic.SubjectId.IsEmpty())
      OutFailure.Diagnostic.SubjectId = RootOwnerPath;
  };
  auto CollectValueFailure = [&](const TSharedPtr<FJsonObject>& Descriptor) {
    AttachDescriptorSource(Descriptor);
    if (ValueDiagnostics.Code.IsEmpty()) FirstValueFailure = OutFailure;
    ValueDiagnostics.Append(OutFailure.Diagnostic);
    OutFailure = FFailure();
  };
  TSet<FString> SeenPaths;
  for (const TSharedPtr<FJsonValue> &DescriptorValue : Prepared.SourceDescriptors) {
    ++DescriptorIndex;
    const TSharedPtr<FJsonObject> Descriptor =
        DescriptorValue.IsValid() && DescriptorValue->Type == EJson::Object
            ? DescriptorValue->AsObject()
            : nullptr;
    ON_SCOPE_EXIT {
      if (!OutFailure.Diagnostic.Code.IsEmpty()) {
        AttachDescriptorSource(Descriptor);
      }
    };
    FString PropertyPath;
    TSharedPtr<FJsonValue> InputValue;
    if (!ValidateDescriptor(DescriptorValue, PropertyPath, InputValue, OutFailure)) {
      if (RootObject)
        Session.ObjectStack.Remove(RootObject);
      return false;
    }
    if (SeenPaths.Contains(PropertyPath)) {
      SetFailure(OutFailure, PropertyFailure(TEXT("duplicate_property_path"), TEXT("The same property path appears more than once in this edit."), PropertyPath, MakeShared<FJsonValueString>(PropertyPath), MakeShared<FJsonValueString>(TEXT("one edit per property"))), FString());
      if (RootObject)
        Session.ObjectStack.Remove(RootObject);
      return false;
    }
    for (const FString &ExistingPath : SeenPaths) {
      if (PropertyPath.StartsWith(ExistingPath + TEXT(".")) ||
          ExistingPath.StartsWith(PropertyPath + TEXT("."))) {
        SetFailure(OutFailure, PropertyFailure(TEXT("overlapping_property_path"), TEXT("The edit addresses both a property and one of its descendants."), PropertyPath, MakeShared<FJsonValueString>(PropertyPath), MakeShared<FJsonValueString>(ExistingPath)), PropertyPath + TEXT(":") + ExistingPath);
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
      SetFailure(OutFailure, TargetResult.Diagnostic, FString(), PropertyPath);
      CollectValueFailure(Descriptor);
      continue;
    }
    if (RootOptions.IncludeRootProperty && !RootOptions.IncludeRootProperty(Target.RootProperty)) {
      SetFailure(OutFailure, PropertyFailure(TEXT("property_outside_domain_scope"), TEXT("The property is outside the fields owned by this domain."), PropertyPath), Target.RootProperty ? Target.RootProperty->GetName() : PropertyPath);
      CollectValueFailure(Descriptor);
      continue;
    }
    if (Target.NotificationOwner && RootObject &&
        !IsObjectWithin(Target.NotificationOwner, RootObject)) {
      SetFailure(OutFailure, PropertyFailure(TEXT("external_reference_recursive_patch_forbidden"), TEXT("Recursive property edits cannot follow a reference outside the requested owner."), PropertyPath), Target.NotificationOwner->GetPathName());
      Session.ObjectStack.Remove(RootObject);
      return false;
    }
    if (Target.NotificationOwner && !RootObject &&
        Target.NotificationOwner != NotificationOwner) {
      SetFailure(OutFailure, PropertyFailure(TEXT("external_reference_recursive_patch_forbidden"), TEXT("Recursive property edits cannot follow a reference outside the requested owner."), PropertyPath), FString());
      return false;
    }

    const TSharedPtr<FJsonObject> InputObject =
        InputValue.IsValid() && InputValue->Type == EJson::Object
            ? InputValue->AsObject()
            : nullptr;
    const bool bExistingInlineDescendantPatch =
        InputObject.IsValid() && InputObject->HasField(TEXT("owned_object"))
        && !IsDangerousProperty(Target.Property)
        && UeAgentAuthoringReflection::IsExistingInlineObjectProperty(
            Target.Property, Target.ValuePtr, Target.NotificationOwner);
    FDiagnostic PropertyReason;
    // A member path cannot bypass its struct/container declaration's access.
    // The chain resets at an owned UObject boundary, whose editable descendants
    // remain independent from replacement of the containing object pointer.
    bool bEditablePath = true;
    for (const FProperty* Member : Target.NotificationChain) {
      if (Member == Target.Property) continue;
      if (!IsEditableForApply(Member, Target.NotificationOwner, Options, PropertyReason)) {
        bEditablePath = false;
        break;
      }
    }
    if (bEditablePath && !bExistingInlineDescendantPatch) {
      bEditablePath = IsEditableForApply(Target.Property, Target.NotificationOwner, Options, PropertyReason, Target.PropertyScope);
    }
    if (!bEditablePath) {
      SetFailure(OutFailure, PropertyReason, Target.Property->GetName(),
                 PropertyPath);
      if (RootObject)
        Session.ObjectStack.Remove(RootObject);
      return false;
    }
    FDiagnostic CodecReason;
    if (!ValidateOptionalTypeAssertions(Target.Property, Descriptor,
                                        CodecReason)) {
      SetFailure(OutFailure, CodecReason, FString(), PropertyPath);
      CollectValueFailure(Descriptor);
      continue;
    }

    FDiagnostic StorageError;
    FPreparedProperty Entry;
    Entry.PropertyPath = PropertyPath;
    Entry.SourceDescriptorIndex = DescriptorIndex;
    Entry.NotificationOwner = Target.NotificationOwner;
    Entry.Property = Target.Property;
    Entry.NotificationChain = MoveTemp(Target.NotificationChain);
    if (!RootObject && !Options.StructNotificationOwnerPropertyPath.IsEmpty()) {
      UeAgentAuthoringTargetLocator::FResolvedPropertyTarget OwnerMember;
      UeAgentAuthoringTargetLocator::FResolveResult OwnerResult;
      if (!NotificationOwner || !UeAgentAuthoringTargetLocator::ResolvePropertyPath(
              NotificationOwner, Options.StructNotificationOwnerPropertyPath, OwnerMember, OwnerResult)
          || OwnerMember.NotificationOwner != NotificationOwner || OwnerMember.NotificationChain.IsEmpty()) {
        auto Diagnostic = OwnerResult.Diagnostic;
        if (Diagnostic.IsEmpty()) {
          Diagnostic = PropertyFailure(TEXT("struct_notification_owner_property_invalid"),
              TEXT("The struct notification property does not resolve to a member of the notification owner."), PropertyPath);
          Diagnostic.SourceKind = TEXT("native_result");
        }
        SetFailure(OutFailure, MoveTemp(Diagnostic), Options.StructNotificationOwnerPropertyPath, PropertyPath);
        return false;
      }
      Entry.NotificationChain = MoveTemp(OwnerMember.NotificationChain);
    }
    Entry.CurrentValuePtr = Target.ValuePtr;
    Entry.DesiredValue = FPropertyValueStorage::CopyFrom(
        Target.Property, Target.ValuePtr, StorageError);
    if (!Entry.DesiredValue.IsValid()) {
      SetFailure(OutFailure, StorageError, FString(), PropertyPath);
      if (RootObject) Session.ObjectStack.Remove(RootObject);
      return false;
    }
    Entry.OriginalValue = FPropertyValueStorage::CopyFrom(
        Target.Property, Target.ValuePtr, StorageError);
    if (!Entry.OriginalValue.IsValid()) {
      SetFailure(OutFailure, StorageError, FString(), PropertyPath);
      if (RootObject)
        Session.ObjectStack.Remove(RootObject);
      return false;
    }

    TSharedPtr<FJsonValue> CompletedValue;
    if (!BuildCompletedValue(Target.Property, InputValue, Target.ValuePtr,
                             Target.NotificationOwner, Prepared, Entry, Options,
                             false, false, Session, PropertyPath,
                             CompletedValue, Entry.Node, OutFailure)) {
      CollectValueFailure(Descriptor);
      continue;
    }
    UeAgentPropertyValueCodec::FOptions CodecOptions =
        MakeCodecOptions(Options);
    // Collection decoding creates element storage anew. Restore each selected
    // native struct baseline there, including noneditable opaque members that
    // cannot be serialized. The codec still owns every authored conversion.
    CodecOptions.ScopedValueInitializer = [&Entry](FProperty* Property,
        const TSharedPtr<FJsonValue>& Value, void* Destination, bool bSingle, FDiagnostic& Error) {
      const auto* Baseline = Entry.NativeStructBaselines.Find(Value.Get());
      if (!Baseline) return true;
      if (!Property->SameType((*Baseline)->GetProperty())) {
        Error = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("native_struct_baseline_type_changed"), Property->GetName(),
            MakeShared<FJsonValueObject>(UeAgentPropertyValueCodec::BuildTypeDescriptor((*Baseline)->GetProperty())),
            MakeShared<FJsonValueObject>(UeAgentPropertyValueCodec::BuildTypeDescriptor(Property)), TEXT("object_apply"),
            TEXT("The native property type differs from the captured struct baseline."));
        Error.SourceKind = TEXT("native_result");
        return false;
      }
      if (bSingle) Property->CopySingleValue(Destination, (*Baseline)->GetValuePtr());
      else Property->CopyCompleteValue(Destination, (*Baseline)->GetValuePtr());
      return true;
    };
    Entry.CompletedValue = CompletedValue;
    if (!UeAgentPropertyValueCodec::Decode(
            Target.Property, CompletedValue,
            Entry.DesiredValue->GetMutableValuePtr(), Target.NotificationOwner,
            CodecOptions, CodecReason)) {
      SetFailure(OutFailure, CodecReason, FString(), PropertyPath);
      CollectValueFailure(Descriptor);
      continue;
    }
    FDiagnostic OwnedReferenceError;
    if (!ValidateOwnedReferences(
            Target.Property, Entry.DesiredValue->GetValuePtr(),
            Target.NotificationOwner, Entry.Materializations, false,
            OwnedReferenceError)) {
      SetFailure(OutFailure, OwnedReferenceError, FString(), PropertyPath);
      if (RootObject)
        Session.ObjectStack.Remove(RootObject);
      return false;
    }
    if (Entry.NestedOwnedPlans.IsEmpty() && Entry.Materializations.IsEmpty()
        && Entry.ExistingInlinePatches.IsEmpty()) {
      bool bEquivalent = false;
      FDiagnostic CompareError;
      if (!UeAgentAuthoringSemanticCompare::ArePropertyValuesEquivalent(
              Target.Property, Target.ValuePtr,
              Entry.DesiredValue->GetValuePtr(), Target.NotificationOwner,
              bEquivalent, CompareError, true)) {
        SetFailure(OutFailure,
                   CompareError.IsEmpty() ? FDiagnostic::Failure(TEXT("property_semantic_compare_failed"), TEXT("The property comparison failed without reporting its diagnostic."), TEXT("object_apply")) : CompareError,
                   PropertyPath, PropertyPath);
        if (RootObject)
          Session.ObjectStack.Remove(RootObject);
        return false;
      }
      const bool bNativeTransition = Target.NotificationOwner && Options.NeedsNativePropertyTransition
          && Options.NeedsNativePropertyTransition(*Target.NotificationOwner, *Target.Property, Entry.Node);
      if (bEquivalent && Entry.TextSourceEdits.IsEmpty() && !bNativeTransition) {
        // Elide the write, not the explicitly requested value. Freeze now so
        // later domain notifications cannot become this value's expectation.
        auto& Value = Prepared.UnchangedValues.AddDefaulted_GetRef();
        Value.Path = Entry.PropertyPath;
        Value.Type = UeAgentPropertyValueCodec::BuildTypeDescriptor(Entry.Property);
        Value.Value = ProjectExplicitIntentValue(Entry.Node,
            UeAgentPropertyValueCodec::Encode(Entry.Property,
                Entry.DesiredValue->GetValuePtr(), Entry.NotificationOwner, CodecOptions));
        const TSharedPtr<FJsonObject>* Source = nullptr;
        if (Descriptor->TryGetObjectField(TEXT("authoring_source"), Source))
          Value.Source = UeAgentAuthoringState::CloneCanonicalValue(MakeShared<FJsonValueObject>(*Source))->AsObject();
        if (!Value.Type || !Value.Value) {
          SetFailure(OutFailure, PropertyFailure(TEXT("property_intent_capture_failed"), TEXT("The requested property type or value could not be captured for verification."), PropertyPath), FString());
          CollectValueFailure(Descriptor);
        }
        continue;
      }
    }
    Prepared.Nodes.Add(Entry.Node);
    Prepared.Properties.Add(MoveTemp(Entry));
  }

  if (!ValueDiagnostics.Code.IsEmpty()) {
    OutFailure = MoveTemp(FirstValueFailure);
    OutFailure.Diagnostic = MoveTemp(ValueDiagnostics);
    const auto Unchecked = MakeShared<FJsonObject>();
    Unchecked->SetStringField(TEXT("stage"), TEXT("object_apply_dependencies"));
    Unchecked->SetStringField(TEXT("reason"), TEXT("invalid_property_values"));
    OutFailure.Diagnostic.Unchecked.Add(MakeShared<FJsonValueObject>(Unchecked));
    if (RootObject) Session.ObjectStack.Remove(RootObject);
    OutPrepared = FPreparedApply();
    return false;
  }
  Prepared.bReady = true;
  if (RootObject)
    Session.ObjectStack.Remove(RootObject);
  return true;
}

bool PrepareRootInternal(UStruct* RootStruct, void* RootData, UObject* RootObject,
    UObject* NotificationOwner, const FString& RootOwnerPath,
    const TArray<TSharedPtr<FJsonValue>>& Descriptors, const FOptions& Options,
    FPrepareSession& Session, FPreparedApply& Prepared, FFailure& Failure) {
  ON_SCOPE_EXIT { SupplementLogicalTarget(Failure.Diagnostic, Options.DiagnosticTargetPath, RootOwnerPath); };
  const bool bOutermost = Session.PreparationDepth == 0;
  TGuardValue<int32> Depth(Session.PreparationDepth, Session.PreparationDepth + 1);
  const auto BeforeStack = Session.ObjectStack;
  ON_SCOPE_EXIT { Session.ObjectStack = BeforeStack; };
  if (PrepareRootOnce(RootStruct, RootData, RootObject, NotificationOwner, RootOwnerPath,
      Descriptors, Options, Session, Prepared, Failure)) return true;
  if (!bOutermost || Session.bCollectValueDiagnostics) return false;
  Prepared = FPreparedApply();
  Session.ObjectStack = BeforeStack;
  TGuardValue<bool> Collect(Session.bCollectValueDiagnostics, true);
  FPreparedApply DiagnosticPlan;
  FFailure DiagnosticFailure;
  if (!PrepareRootOnce(RootStruct, RootData, RootObject, NotificationOwner, RootOwnerPath,
      Descriptors, Options, Session, DiagnosticPlan, DiagnosticFailure)
      && !DiagnosticFailure.Diagnostic.Code.IsEmpty()) Failure = MoveTemp(DiagnosticFailure);
  return false;
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
    OutResult.Diagnostic = FDiagnostic::Failure(TEXT("owned_object_real_outer_missing"), TEXT("The owner for materializing the owned object is unavailable."), TEXT("object_apply"));
    OutResult.Diagnostic.InternalContext = Entry.PropertyPath;
    OutResult.Diagnostic.PropertyPath = Entry.PropertyPath;
    return false;
  }
  const bool bNativeCreation = Entry.Materializations.ContainsByPredicate(
      [](const FMaterialization& Item) { return Item.NativeContext.IsValid(); });
  auto ValidateNativeContext = [&](FMaterialization& Item, FNativeOwnedCreationContext& Context, bool bProspective)
  {
    FDiagnostic Error;
    TArray<FOwnedClassCandidate> Candidates;
    const bool bInitialized = Item.ScratchObject.IsValid() && (Item.NativeScopedProperty
        ? Context.InitializeScopedMember(Outer, Item.NativePropertyPath, Item.NativeScopedProperty, bProspective, Error)
        : Context.Initialize(Outer, Item.NativePropertyPath, bProspective, Error, Item.NativeIndex));
    if (!bInitialized
        || !Context.Collect(Candidates, Error)) {
      OutResult.Diagnostic = Error.IsEmpty() ? FDiagnostic::Failure(TEXT("owned_object_scratch_expired"), TEXT("The prepared owned object expired before it could be applied."), TEXT("object_apply")) : Error;
      OutResult.Diagnostic.PropertyPath = Entry.PropertyPath; return false;
    }
    TArray<UE::UAI::ContentDiscovery::FNameSelectionEntry> Names;
    for (const auto& Candidate : Candidates) Names.Add(Candidate.Selection);
    int32 Selected = INDEX_NONE;
    if (!UE::UAI::ContentDiscovery::TryResolveNameSelection(Names,
        Item.NativeSelector.Name, Item.NativeSelector.Select, Selected, Error)
        || Candidates[Selected].Class != Item.ScratchObject->GetClass()) {
      OutResult.Diagnostic = Error.IsEmpty() ? FDiagnostic::Failure(TEXT("owned_candidate_context_changed"), TEXT("The native creation context changed after the owned object candidate was prepared."), TEXT("object_apply")) : Error;
      OutResult.Diagnostic.PropertyPath = Entry.PropertyPath; return false;
    }
    return true;
  };
  if (bNativeCreation) {
    // Re-resolve every planned native creation against the current owner before
    // Modify or any real slot write. Prepare-time eligibility can become stale.
    for (FMaterialization& Item : Entry.Materializations) {
      if (!Item.NativeContext) continue;
      FNativeOwnedCreationContext Current;
      if (!ValidateNativeContext(Item, Current, true)) return false;
    }
    // The native helper writes its real property handle. Register the original
    // property and dirty state before exposing the prospective storage.
    FDiagnostic StorageError;
    auto Slots = FPropertyValueStorage::CreateInitialized(Entry.Property, StorageError);
    if (!Slots) { OutResult.Diagnostic = StorageError; return false; }
    Entry.Property->CopyCompleteValue(Slots->GetMutableValuePtr(), Entry.DesiredValue->GetValuePtr());
    if (!ReplaceScratchReferences(Entry.Property, Slots->GetMutableValuePtr(),
            Entry.Materializations, false, StorageError)) {
      OutResult.Diagnostic = StorageError.IsEmpty() ? FDiagnostic::Failure(TEXT("owned_object_prospective_clear_failed"), TEXT("The prospective owned object could not be cleared before materialization."), TEXT("object_apply")) : StorageError;
      return false;
    }
    RecordPackageState(Prepared, Outer);
    if (Prepared.Options.bRecordOwnerTransaction)
      Outer->Modify(Prepared.Options.bMarkPackageDirty);
    Entry.bPropertyApplied = true;
    Entry.Property->CopyCompleteValue(Entry.CurrentValuePtr, Slots->GetValuePtr());
  }
  for (FMaterialization &Materialization : Entry.Materializations) {
    UObject *ScratchObject = Materialization.ScratchObject.Get();
    if (!ScratchObject) {
      OutResult.Diagnostic = FDiagnostic::Failure(TEXT("owned_object_scratch_expired"), TEXT("The prepared owned object expired before it could be applied."), TEXT("object_apply"));
      OutResult.Diagnostic.InternalContext = Entry.PropertyPath;
      OutResult.Diagnostic.PropertyPath = Entry.PropertyPath;
      return false;
    }
    UObject* Created = nullptr;
    if (Materialization.NativeContext) {
      FNativeOwnedCreationContext ActualContext;
      if (!ValidateNativeContext(Materialization, ActualContext, false)) return false;
      FDiagnostic Error;
      Created = ActualContext.Create(ScratchObject->GetClass(), Error);
      // Capture the native result even when its postcondition fails, so the
      // existing rollback owns every object created by the helper.
      Materialization.CreatedObject.Reset(Created);
      if (!Error.IsEmpty()) {
        OutResult.Diagnostic = Error; OutResult.Diagnostic.PropertyPath = Entry.PropertyPath; return false;
      }
    } else {
      Created = StaticDuplicateObject(ScratchObject, Outer, NAME_None, RF_Transactional);
    }
    if (!Created) {
      OutResult.Diagnostic = FDiagnostic::Failure(TEXT("owned_object_create_failed"), TEXT("The native owner could not create the requested owned object."), TEXT("object_apply"));
      OutResult.Diagnostic.InternalContext = Entry.PropertyPath;
      OutResult.Diagnostic.PropertyPath = Entry.PropertyPath;
      return false;
    }
    Materialization.CreatedObject.Reset(Created);
    if ((Materialization.NativeContext && !Materialization.SourceProperties.IsEmpty())
        || (Materialization.bHasTextSourceEdits && !Prepared.bDeferTextSourceEdits)) {
      auto Nested = MakeUnique<FPreparedApply>();
      FFailure Failure;
      FPrepareSession Session;
      Session.bUniqueDescriptorOccurrences = true;
      Session.bDeferTextSourceEdits = Prepared.bDeferTextSourceEdits;
      if (!PrepareRootInternal(Created->GetClass(), Created, Created, Created, Created->GetPathName(),
          Materialization.SourceProperties, Prepared.Options, Session, *Nested, Failure)) {
        OutResult.Diagnostic = Failure.Diagnostic;
        return false;
      }
      Nested->SetTextIdentityPackage(Prepared.TextIdentityPackage);
      Entry.NestedOwnedPlans.Add(MoveTemp(Nested));
    }
  }
  FDiagnostic ReplaceError;
  if (!ReplaceScratchReferences(Entry.Property,
                                Entry.DesiredValue->GetMutableValuePtr(),
                                Entry.Materializations, false, ReplaceError)) {
    OutResult.Diagnostic = FDiagnostic::Failure(TEXT("owned_object_materialization_replace_failed"), TEXT("The materialized object could not replace its prepared reference."), TEXT("object_apply"));
    OutResult.Diagnostic.PublicCause = MakeShared<FDiagnostic>(MoveTemp(ReplaceError));
    OutResult.Diagnostic.PropertyPath = Entry.PropertyPath;
    return false;
  }
  return true;
}

using FNotificationChains = TArray<TArray<FProperty *>>;

void GatherChangedNotifications(FProperty* Property, const void* Before, const void* After,
                                TArray<FProperty*>& Chain, FNotificationChains& Out,
                                UObject* Owner, const UeAgentPropertyValueCodec::FOptions& Options) {
  // Notification selection must see the same authored differences as preparation.
  // Native Identical alone folds case-only string edits, including nested values.
  if (UeAgentPropertyValueCodec::AreSemanticallyEquivalent(Property, Before, After, Owner, Options)) return;
  const FStructProperty* Struct = CastField<FStructProperty>(Property);
  if (Struct && Property->ArrayDim == 1
      && !(Struct->Struct->StructFlags & STRUCT_IdenticalNative)
      && UeAgentAuthoringTypeDescriptors::GetStructKind(Struct->Struct) == TEXT("struct")) {
    bool bHasFields = false;
    for (TFieldIterator<FProperty> It(Struct->Struct); It; ++It) {
      if (It->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated)) continue;
      bHasFields = true;
      Chain.Add(*It);
      GatherChangedNotifications(*It, It->ContainerPtrToValuePtr<void>(Before),
                                  It->ContainerPtrToValuePtr<void>(After), Chain, Out, Owner, Options);
      Chain.Pop();
    }
    if (bHasFields) return;
  }
  // Containers are replaced as a whole. Do not fabricate element indices or array operations.
  Out.Add(Chain);
}

FNotificationChains BuildNotifications(const FPreparedProperty& Entry, const void* Desired,
                                       bool bEnabled, const FOptions& Options) {
  FNotificationChains Result;
  if (!bEnabled || !Entry.NotificationOwner || Entry.NotificationChain.IsEmpty()) return Result;
  const UClass* MemberClass = Cast<UClass>(Entry.NotificationChain[0]->GetOwnerStruct());
  if (!MemberClass || !Entry.NotificationOwner->IsA(MemberClass)) return Result;
  auto Chain = Entry.NotificationChain;
  if (Options.NeedsNativePropertyTransition
      && Options.NeedsNativePropertyTransition(*Entry.NotificationOwner, *Entry.Property, Entry.Node)) {
    Result.Add(Chain);
    return Result;
  }
  // A domain may identify only the containing member for independently
  // addressed struct storage. Detect the scoped change, then notify that member
  // without appending a fabricated path through an array/instanced struct.
  if (Chain.Last() != Entry.Property) {
    if (!UeAgentPropertyValueCodec::AreSemanticallyEquivalent(Entry.Property, Entry.CurrentValuePtr,
        Desired, Entry.NotificationOwner, MakeCodecOptions(Options, true))) Result.Add(Chain);
    return Result;
  }
  GatherChangedNotifications(Entry.Property, Entry.CurrentValuePtr, Desired, Chain, Result,
      Entry.NotificationOwner, MakeCodecOptions(Options, true));
  return Result;
}

bool NotifyPropertyChains(UObject& Owner, const FNotificationChains& Notifications, bool bPre,
    EPropertyChangeType::Type ChangeType = EPropertyChangeType::ValueSet,
    const TFunction<bool(UObject&, FPropertyChangedEvent&)>& PostEditNotification = {},
    const TFunction<bool(UObject&, FPropertyChangedEvent&, FFailure&)>& CheckedNotification = {},
    FFailure* OutFailure = nullptr) {
  FNotificationChains Ordered;
  if (!bPre && !Notifications.IsEmpty())
    UeAgentBlackboardKeySelectorValue::Resolve(Owner, Ordered);
  Ordered.Append(Notifications);
  for (const auto& Properties : Ordered) {
    FEditPropertyChain Chain;
    for (FProperty* Property : Properties) Chain.AddTail(Property);
    Chain.SetActiveMemberPropertyNode(Properties[0]);
    Chain.SetActivePropertyNode(Properties.Last());
    if (bPre) Owner.PreEditChange(Chain);
    else {
      FPropertyChangedEvent Event(Properties.Last(), ChangeType);
      Event.SetActiveMemberProperty(Properties[0]);
      FPropertyChangedChainEvent ChainEvent(Chain, Event);
      if (CheckedNotification) {
        FFailure Failure;
        if (!CheckedNotification(Owner, ChainEvent, Failure)) {
          if (Failure.Diagnostic.Code.IsEmpty()) Failure.Diagnostic = FDiagnostic::Failure(TEXT("native_notification_failed"), TEXT("The native property notification failed without reporting its diagnostic."), TEXT("object_apply"));
          if (Failure.Diagnostic.PropertyPath.IsEmpty()) Failure.Diagnostic.PropertyPath = Properties[0]->GetName();
          if (OutFailure) *OutFailure = MoveTemp(Failure);
          return false;
        }
      } else if (!PostEditNotification || !PostEditNotification(Owner, ChainEvent))
        Owner.PostEditChangeChainProperty(ChainEvent);
    }
  }
  return true;
}

bool NotifyWholeObject(UObject& Owner, const FOptions& Options, FFailure& Failure,
    bool bUseObjectHook) {
  FPropertyChangedEvent Event(nullptr, Options.NotificationChangeType);
  if (Options.CheckedPostEditNotification) {
    if (Options.CheckedPostEditNotification(Owner, Event, Failure)) return true;
    if (Failure.Diagnostic.Code.IsEmpty()) Failure.Diagnostic = FDiagnostic::Failure(TEXT("native_notification_failed"), TEXT("The native property notification failed without reporting its diagnostic."), TEXT("object_apply"));
    
    return false;
  }
  if (Options.PostEditNotification && Options.PostEditNotification(Owner, Event)) return true;
  // UE's InputAction callback requires a member even for a newly published
  // object. Refresh its three native change channels through the same member
  // notification owner used by property edits, without sending a null event.
  if (Owner.IsA<UInputAction>())
    return NotifyObjectMembers(Owner, {GET_MEMBER_NAME_CHECKED(UInputAction, ValueType),
        GET_MEMBER_NAME_CHECKED(UInputAction, Modifiers), GET_MEMBER_NAME_CHECKED(UInputAction, Triggers)}, false, Failure);
  if (bUseObjectHook) Owner.PostEditChange();
  else Owner.PostEditChangeProperty(Event);
  return true;
}

bool NotifyPropertyBatch(UObject& Owner, const FNotificationChains& Notifications,
    const FOptions& Options, bool bWholeObject, FFailure& Failure) {
  if (bWholeObject) return NotifyWholeObject(Owner, Options, Failure, false);
  auto Ordered = Notifications;
  if (Owner.IsA<UMeshComponent>()) {
    // SkeletalMeshAsset is an editor alias: its callback updates SkinnedAsset.
    // Material-slot callbacks must see that resolved mesh, even though all raw
    // fields have already been written. Keep unrelated notifications stable.
    Ordered.StableSort([](const auto& A, const auto& B) {
      const auto Priority = [](const auto& Chain) {
        const FName Member = Chain[0]->GetFName();
        return Member == TEXT("StaticMesh") || Member == TEXT("SkeletalMeshAsset")
            || Member == TEXT("SkinnedAsset") ? 0 : 1;
      };
      return Priority(A) < Priority(B);
    });
  }
  auto* Skeletal = Cast<USkeletalMeshComponent>(&Owner);
  const bool bAnimationDataChanged = Skeletal && Ordered.ContainsByPredicate([](const auto& Chain) {
    return Chain[0]->GetFName() == GET_MEMBER_NAME_CHECKED(USkeletalMeshComponent, AnimationData);
  });
  const auto DesiredMode = Skeletal ? Skeletal->GetAnimationMode() : EAnimationMode::AnimationCustomMode;
  if (!NotifyPropertyChains(Owner, Ordered, false, Options.NotificationChangeType,
      Options.PostEditNotification, Options.CheckedPostEditNotification, &Failure)) return false;
  if (bAnimationDataChanged) {
    // The native AnimToPlay leaf event calls PlayAnimation, which selects
    // single-node mode and non-looping playback. HJSON edits persistent intent,
    // including inactive animation data, rather than issuing that playback action.
    // Retain native validation, then initialize from the complete saved intent.
    if (Skeletal->GetAnimationMode() != DesiredMode) Skeletal->SetAnimationMode(DesiredMode);
    Skeletal->InitAnim(true);
  }
  return true;
}

void SetNotificationFailure(FApplyResult& Result, const FFailure& Failure) {
  Result.Diagnostic = Failure.Diagnostic;
}

bool ApplyPreparedInternal(FPreparedApply::FImpl &Prepared, FApplyResult &OutResult);
struct FVerificationRoot;
bool VerifyPreparedInternal(FPreparedApply::FImpl &Prepared, FApplyResult &OutResult,
                            const FVerificationRoot* VerificationRoot = nullptr);
bool RefreshPropertyTarget(FPreparedApply::FImpl& Prepared,
                           FPreparedProperty& Entry, FApplyResult& Result,
                           const FVerificationRoot* VerificationRoot = nullptr);

bool ApplyNativeStages(FPreparedApply::FImpl& Prepared, FApplyResult& OutResult) {
  TArray<TArray<TSharedPtr<FJsonValue>>> Stages;
  Stages.SetNum(Prepared.Options.NativeStages.Num() + 1);
  for (const auto& Descriptor : Prepared.SourceDescriptors) {
    const FString Path = Descriptor->AsObject()->GetStringField(TEXT("property_path"));
    FString Root = Path; int32 End;
    if (Root.FindChar(TCHAR('.'), End)) Root.LeftInline(End);
    if (Root.FindChar(TCHAR('['), End)) Root.LeftInline(End);
    int32 Stage = Prepared.Options.NativeStages.Num();
    for (int32 Index = 0; Index < Prepared.Options.NativeStages.Num(); ++Index)
      if (Prepared.Options.NativeStages[Index].RootProperties.Contains(FName(*Root))) { Stage = Index; break; }
    Stages[Stage].Add(Descriptor);
  }
  Prepared.Properties.Reset(); Prepared.Nodes.Reset();
  Prepared.UnchangedValues.Reset(); Prepared.UnchangedOwnedIntents.Reset();
  auto Options = Prepared.Options; Options.NativeStages.Reset();
  for (int32 Index = 0; Index < Stages.Num(); ++Index) {
    if (Stages[Index].IsEmpty()) continue;
    auto Plan = MakeUnique<FPreparedApply>(); FPrepareSession Session;
    Session.bDeferTextSourceEdits = Prepared.bDeferTextSourceEdits;
    FFailure Failure;
    if (!PrepareRootInternal(Prepared.RootStruct, Prepared.RootData, Prepared.RootObject,
        Prepared.NotificationOwner, Prepared.RootOwnerPath, Stages[Index], Options, Session, *Plan, Failure)) {
      OutResult.Diagnostic = Failure.Diagnostic; return false;
    }
    auto& Stage = FPreparedApplyAccess::Get(*Plan);
    Stage.TextIdentityPackage = Prepared.TextIdentityPackage;
    Stage.bBatchNotifications = Index < Prepared.Options.NativeStages.Num();
    Stage.bWholeObjectNotification = Stage.bBatchNotifications && Prepared.Options.NativeStages[Index].bWholeObjectNotification;
    FApplyResult Result;
    const bool bApplied = ApplyPreparedInternal(Stage, Result);
    if (bApplied && Prepared.Options.bVerifyNativeStages) {
      FApplyResult Observed;
      if (!VerifyPreparedInternal(Stage, Observed))
        UeAgentAuthoringDiagnostics::ReportConsistencyWarning(Observed.Diagnostic.Code,
            Observed.Diagnostic, TEXT("native_stage_observation"));
    }
    for (const auto& Dirty : Stage.PackageDirtyStates)
      if (!Prepared.PackageDirtyStates.Contains(Dirty.Key)) Prepared.PackageDirtyStates.Add(Dirty.Key, Dirty.Value);
    for (auto& Pair : Stage.TextMetadataBefore) {
      auto& Before = Prepared.TextMetadataBefore.FindOrAdd(Pair.Key);
      for (const auto& Field : Pair.Value) if (!Before.Contains(Field.Key)) Before.Add(Field.Key, Field.Value);
    }
    Prepared.PropertiesAttempted += Stage.PropertiesAttempted;
    OutResult.PropertiesAttempted = Prepared.PropertiesAttempted;
    OutResult.PropertiesApplied += Result.PropertiesApplied;
    Prepared.Nodes.Append(Stage.Nodes);
    Prepared.Properties.Append(MoveTemp(Stage.Properties));
    Prepared.UnchangedValues.Append(MoveTemp(Stage.UnchangedValues));
    Prepared.UnchangedOwnedIntents.Append(MoveTemp(Stage.UnchangedOwnedIntents));
    Prepared.StageStorage.Add(MoveTemp(Plan));
    if (!bApplied) { OutResult.Diagnostic = Result.Diagnostic; return false; }
  }
  Prepared.bApplied = true;
  OutResult.Status = TEXT("applied");
  OutResult.PropertiesPrepared = Prepared.Properties.Num();
  return true;
}

void SupplementPreparedInputSource(FDiagnostic& Diagnostic, const FPreparedApply::FImpl& Prepared,
                                  const FPreparedProperty& Entry) {
  if (Diagnostic.SourceKind == TEXT("authored_value") || Diagnostic.SourceKind == TEXT("command_input")
      || Diagnostic.SourceKind == TEXT("related_input")) {
    if (!Prepared.SourceDescriptors.IsValidIndex(Entry.SourceDescriptorIndex)) return;
    const auto& Value = Prepared.SourceDescriptors[Entry.SourceDescriptorIndex];
    if (!Value.IsValid() || Value->Type != EJson::Object) return;
    UeAgentAuthoringDiagnostics::LocatePropertyValueSource(Diagnostic, *Value->AsObject());
    if (Prepared.Options.SupplementDiagnosticSource)
      Prepared.Options.SupplementDiagnosticSource(Diagnostic, *Value->AsObject(), Prepared.RootStruct,
          Prepared.RootData, Entry.SourceDescriptorIndex);
    return;
  }
  for (auto& Child : Diagnostic.Diagnostics) SupplementPreparedInputSource(Child, Prepared, Entry);
  if (Diagnostic.PublicCause) {
    auto Cause = MakeShared<FDiagnostic>(*Diagnostic.PublicCause);
    SupplementPreparedInputSource(*Cause, Prepared, Entry); Diagnostic.PublicCause = MoveTemp(Cause);
  }
}

bool ApplyPreparedInternal(FPreparedApply::FImpl &Prepared,
                           FApplyResult &OutResult) {
  ON_SCOPE_EXIT {
    if (!OutResult.Diagnostic.IsEmpty() && OutResult.Diagnostic.SourceKind.IsEmpty())
      OutResult.Diagnostic.SourceKind = TEXT("native_result");
    SupplementLogicalTarget(OutResult.Diagnostic, Prepared.Options.DiagnosticTargetPath, Prepared.RootOwnerPath);
  };
  OutResult = FApplyResult();
  OutResult.Status = TEXT("failed");
  OutResult.PropertiesPrepared = Prepared.Properties.Num();
  if (!Prepared.bReady || Prepared.bApplied || Prepared.bRolledBack) {
    OutResult.Diagnostic = FDiagnostic::Failure(TEXT("prepared_apply_state_invalid"), TEXT("The property plan is not ready, is already applied, or has been rolled back."), TEXT("object_apply"));
    OutResult.Diagnostic.InternalContext = Prepared.RootOwnerPath;
    return false;
  }
  if (!Prepared.Options.NativeStages.IsEmpty()) return ApplyNativeStages(Prepared, OutResult);

  // Resolve the native asset context before dependent edits, including a loaded
  // node whose transient key cache was initialized without a Details event.
  if (Prepared.Options.bEmitEditorNotifications && !Prepared.bWholeObjectNotification) {
    TSet<UObject*> Owners;
    if (Prepared.RootObject && !Prepared.SourceDescriptors.IsEmpty()) Owners.Add(Prepared.RootObject);
    for (const auto& Entry : Prepared.Properties)
      if (Entry.NotificationOwner) Owners.Add(Entry.NotificationOwner);
    for (UObject* Owner : Owners) {
      if (UeAgentBlackboardKeySelectorValue::RootProperties(*Owner).IsEmpty()) continue;
      RecordPackageState(Prepared, Owner);
      if (Prepared.Options.bRecordOwnerTransaction) Owner->Modify(Prepared.Options.bMarkPackageDirty);
      FNotificationChains Resolved;
      UeAgentBlackboardKeySelectorValue::Resolve(*Owner, Resolved, true);
      FFailure Failure;
      if (!NotifyPropertyChains(*Owner, Resolved, false, Prepared.Options.NotificationChangeType,
          Prepared.Options.PostEditNotification, Prepared.Options.CheckedPostEditNotification, &Failure)) {
        SetNotificationFailure(OutResult, Failure);
        return false;
      }
    }
  }

  TMap<UObject*, FNotificationChains> DeferredNotifications;
  if (Prepared.bBatchNotifications && Prepared.Options.bEmitEditorNotifications) {
    for (const auto& Entry : Prepared.Properties) if (Entry.NotificationOwner && !Entry.bDescendantsOnly) {
      RecordPackageState(Prepared, Entry.NotificationOwner);
      DeferredNotifications.FindOrAdd(Entry.NotificationOwner).Append(BuildNotifications(Entry, Entry.DesiredValue->GetValuePtr(), true, Prepared.Options));
    }
    for (const auto& Pair : DeferredNotifications) {
      if (Prepared.Options.bRecordOwnerTransaction)
        Pair.Key->Modify(Prepared.Options.bMarkPackageDirty);
      if (Prepared.bWholeObjectNotification) Pair.Key->PreEditChange(static_cast<FProperty*>(nullptr));
      else NotifyPropertyChains(*Pair.Key, Pair.Value, true);
    }
  }

  for (int32 Index = 0; Index < Prepared.Properties.Num(); ++Index) {
    FPreparedProperty &Entry = Prepared.Properties[Index];
    ON_SCOPE_EXIT {
      if (!OutResult.Diagnostic.IsEmpty()) SupplementPreparedInputSource(OutResult.Diagnostic, Prepared, Entry);
    };
    Prepared.PropertiesAttempted = Index + 1;
    OutResult.PropertiesAttempted = Prepared.PropertiesAttempted;
    if (Entry.bDescendantsOnly)
      continue;
    // An earlier container write or native notification can replace the storage
    // resolved during preparation. Keep the decoded intent, resolve its current
    // destination before reading or writing through the prepared address.
    if (!RefreshPropertyTarget(Prepared, Entry, OutResult)) return false;
    if (!Prepared.bDeferTextSourceEdits && !Entry.TextSourceEdits.IsEmpty()) {
      UPackage* Package = Prepared.TextIdentityPackage ? Prepared.TextIdentityPackage
          : Entry.NotificationOwner ? Entry.NotificationOwner->GetPackage() : nullptr;
      if (!Package) {
        OutResult.Diagnostic = FDiagnostic::Failure(TEXT("text_source_identity_package_missing"), TEXT("The package required to assign the edited text identity is unavailable."), TEXT("object_apply"));
        OutResult.Diagnostic.InternalContext = Entry.PropertyPath;
        return false;
      }
      Prepared.PackageDirtyStates.FindOrAdd(Package, Package->IsDirty());
      for (auto& Edit : Entry.TextSourceEdits) {
        FText Desired;
        if (!Edit.Source.IsEmpty()) {
          const auto MetadataBefore = Package->GetMetaData().RootMetaDataMap;
          const FTextId BeforeId = FTextInspector::GetTextId(Edit.Before);
          FString Namespace, Key;
          TextNamespaceUtil::GetTextIdForEdit(Package, TextNamespaceUtil::ETextEditAction::SourceString,
              Edit.Source, BeforeId.GetNamespace().ToString(), BeforeId.GetKey().ToString(), Namespace, Key);
          // Record only metadata entries actually written by the native editor API.
          auto& Before = Prepared.TextMetadataBefore.FindOrAdd(Package);
          for (const auto& Pair : Package->GetMetaData().RootMetaDataMap) {
            const FString* Old = MetadataBefore.Find(Pair.Key);
            if ((!Old || !Old->Equals(Pair.Value, ESearchCase::CaseSensitive)) && !Before.Contains(Pair.Key))
              Before.Add(Pair.Key, Old ? TOptional<FString>(*Old) : TOptional<FString>());
          }
          Desired = FText::AsLocalizable_Advanced(Namespace, Key, Edit.Source);
        }
        const auto Encoded = UeAgentPropertyValueCodec::Encode(Edit.Property, &Desired,
            Entry.NotificationOwner, MakeCodecOptions(Prepared.Options), 0, nullptr, true);
        Edit.Completed->Values = Encoded->AsObject()->Values;
      }
      FDiagnostic Error;
      if (!UeAgentPropertyValueCodec::Decode(Entry.Property, Entry.CompletedValue,
          Entry.DesiredValue->GetMutableValuePtr(), Entry.NotificationOwner,
          MakeCodecOptions(Prepared.Options), Error)) {
        OutResult.Diagnostic = Error;
        OutResult.Diagnostic.InternalContext = Entry.PropertyPath;
        return false;
      }
    }
    if (!MaterializeEntry(Prepared, Entry, OutResult))
      return false;

    TSharedPtr<FPropertyValueStorage> MergedValue;
    const void* WriteValue = Entry.DesiredValue->GetValuePtr();
    if (Entry.Node.ValueKind == TEXT("struct")) {
      MergedValue = FPropertyValueStorage::CopyFrom(Entry.Property, Entry.CurrentValuePtr, OutResult.Diagnostic);
      if (!MergedValue || !CopyExplicitPropertyValue(Entry.Property, WriteValue,
          MergedValue->GetMutableValuePtr(), Entry.Node, OutResult.Diagnostic)) {
        OutResult.Diagnostic.PropertyPath = Entry.PropertyPath; return false;
      }
      WriteValue = MergedValue->GetValuePtr();
    }

    UObject *Owner = Entry.NotificationOwner;
    RecordPackageState(Prepared, Owner);
    if (Owner && Prepared.Options.bRecordOwnerTransaction)
      Owner->Modify(Prepared.Options.bMarkPackageDirty);
    const auto Notifications = BuildNotifications(Entry, WriteValue, Prepared.Options.bEmitEditorNotifications, Prepared.Options);
    TArray<UeAgentTemplatePropertyPropagation::FInheritedField> InheritedFields;
    if (Owner && Owner->IsTemplate() && !Entry.NotificationChain.IsEmpty()
        && !UeAgentPropertyValueCodec::AreSemanticallyEquivalent(Entry.Property, Entry.CurrentValuePtr,
            WriteValue, Owner, MakeCodecOptions(Prepared.Options, true))) {
      FProperty* Member = Entry.NotificationChain[0];
      const auto* MemberClass = Cast<UClass>(Member->GetOwnerStruct());
      if (MemberClass && Owner->IsA(MemberClass)) {
        // Nested owned-object descriptors may include a prefix relative to the
        // root. Accept only a suffix proven to address this exact owner's storage.
        FString CandidatePath = Entry.PropertyPath;
        while (!CandidatePath.IsEmpty()) {
          UeAgentAuthoringTargetLocator::FResolvedPropertyTarget Target;
          UeAgentAuthoringTargetLocator::FResolveResult Resolution;
          if (UeAgentAuthoringTargetLocator::ResolvePropertyPath(Owner, CandidatePath, Target, Resolution)
              && Target.NotificationOwner == Owner && Target.ValuePtr == Entry.CurrentValuePtr) {
            // Use the same changed-field boundaries as native notifications.
            // Comparing the entire struct would treat an unrelated instance
            // override as an override of every field in this template edit.
            const auto ChangedFields = Prepared.Options.bEmitEditorNotifications
                ? Notifications : BuildNotifications(Entry, WriteValue, true, Prepared.Options);
            for (const auto& Chain : ChangedFields) {
              FString FieldPath = CandidatePath;
              const int32 PropertyIndex = Chain.Find(Entry.Property);
              if (PropertyIndex != INDEX_NONE)
                for (int32 FieldIndex = PropertyIndex + 1; FieldIndex < Chain.Num(); ++FieldIndex)
                  FieldPath += TEXT(".") + Chain[FieldIndex]->GetName();
              auto& Field = InheritedFields.AddDefaulted_GetRef();
              Field.PropertyPath = MoveTemp(FieldPath);
              UeAgentTemplatePropertyPropagation::Gather(*Owner, *Member, Field.PropertyPath,
                  Prepared.InheritedInstanceValues, Field.Instances);
            }
            break;
          }
          int32 Dot;
          if (!CandidatePath.FindChar(TEXT('.'), Dot)) break;
          CandidatePath.RightChopInline(Dot + 1);
        }
      }
    }
    if (Owner && !Prepared.bBatchNotifications) NotifyPropertyChains(*Owner, Notifications, true);
    if (Owner && Prepared.Options.NativePropertyTransition)
      Prepared.Options.NativePropertyTransition(*Owner, *Entry.Property, Entry.Node, false);
    if (const auto* Trace = UeAgentEnvTraceDataValue::LinkedMemberContainer(Entry.Property, Entry.CurrentValuePtr))
      Entry.LinkedTraceBefore = MakeUnique<FEnvTraceData>(*Trace);
    if (!CopyExplicitPropertyValue(Entry.Property, Entry.DesiredValue->GetValuePtr(),
        Entry.CurrentValuePtr, Entry.Node, OutResult.Diagnostic)) {
      OutResult.Diagnostic.PropertyPath = Entry.PropertyPath; return false;
    }
    Entry.bPropertyApplied = true;
    UeAgentEnvTraceDataValue::FinalizeLinkedMember(Entry.Property, Entry.CurrentValuePtr);
    if (!InheritedFields.IsEmpty() && !UeAgentTemplatePropertyPropagation::Apply(
        *Owner, InheritedFields, OutResult.Diagnostic)) {
      OutResult.Diagnostic.PropertyPath = Entry.PropertyPath;
      return false;
    }
    if (auto* Spline = Cast<USplineComponent>(Owner)) {
      const auto* StructProperty = CastField<FStructProperty>(Entry.Property);
      if (StructProperty && StructProperty->Struct == FSplineCurves::StaticStruct()
          && Entry.Property->GetFName() == GET_MEMBER_NAME_CHECKED(USplineComponent, SplineCurves)) {
        Prepared.SplineEditedBefore.FindOrAdd(Spline, Spline->bSplineHasBeenEdited);
        Spline->SetSpline(*static_cast<const FSplineCurves*>(Entry.CurrentValuePtr));
        Spline->SetOverrideConstructionScript(true);
        Spline->UpdateSpline();
      }
    }
    if (Owner && !Prepared.bBatchNotifications) {
      FFailure Failure;
      if (!NotifyPropertyChains(*Owner, Notifications, false, Prepared.Options.NotificationChangeType,
          Prepared.Options.PostEditNotification, Prepared.Options.CheckedPostEditNotification, &Failure)) {
        SetNotificationFailure(OutResult, Failure);
        return false;
      }
    }

    if (Prepared.Options.bMarkPackageDirty && Owner)
      Owner->MarkPackageDirty();
    OutResult.PropertiesApplied = Index + 1;
  }

  for (const auto& Pair : Prepared.SplineEditedBefore) {
    Pair.Key->SetOverrideConstructionScript(true);
    Pair.Key->UpdateSpline();
  }
  for (const auto& Pair : DeferredNotifications) {
    FFailure Failure;
    if (!NotifyPropertyBatch(*Pair.Key, Pair.Value, Prepared.Options, Prepared.bWholeObjectNotification, Failure)) {
      SetNotificationFailure(OutResult, Failure);
      return false;
    }
  }

  // Native notifications on one root field may replace a derived inline
  // object owned by another field. Resolve every existing-inline descendant
  // after all direct fields have reached their native post-change state.
  for (FPreparedProperty &Entry : Prepared.Properties) {
    for (const FExistingInlinePatch& Patch : Entry.ExistingInlinePatches) {
      UeAgentAuthoringTargetLocator::FResolvedPropertyTarget Target;
      UeAgentAuthoringTargetLocator::FResolveResult ResolveResult;
      if (!UeAgentAuthoringTargetLocator::ResolvePropertyPath(
          Prepared.RootStruct, Prepared.RootData, Prepared.RootOwnerPath,
          Prepared.NotificationOwner, Patch.PropertyPath, Target, ResolveResult)
          || !UeAgentAuthoringReflection::IsExistingInlineObjectProperty(
              Target.Property, Target.ValuePtr, Target.NotificationOwner)) {
        OutResult.Diagnostic = ResolveResult.Diagnostic.IsEmpty()
            ? FDiagnostic::Failure(TEXT("existing_inline_object_binding_lost"), TEXT("The existing inline object binding changed or disappeared before the deferred edit."), TEXT("object_apply"))
            : ResolveResult.Diagnostic;
        if (OutResult.Diagnostic.PropertyPath.IsEmpty()) OutResult.Diagnostic.PropertyPath = Patch.PropertyPath;
        return false;
      }
      FObjectPropertyBase* ObjectProperty = CastFieldChecked<FObjectPropertyBase>(Target.Property);
      UObject* CurrentObject = ObjectProperty->GetObjectPropertyValue(Target.ValuePtr);
      FPrepareSession Session;
      Session.bUniqueDescriptorOccurrences = true;
      Session.bDeferTextSourceEdits = Prepared.bDeferTextSourceEdits;
      TArray<TSharedPtr<FJsonValue>> Properties = Patch.Properties;
      FFailure Failure;
      TUniquePtr<FPreparedApply> Deferred = MakeUnique<FPreparedApply>();
      if (!ResolveReadableOwnedProperties(CurrentObject, Patch.ReadableProperties,
              Prepared.Options, Entry.PropertyPath, Properties, Failure)
          || !PrepareRootInternal(CurrentObject->GetClass(), CurrentObject, CurrentObject,
              CurrentObject, CurrentObject->GetPathName(), Properties, Prepared.Options,
              Session, *Deferred, Failure)) {
        OutResult.Diagnostic = Failure.Diagnostic;
        if (OutResult.Diagnostic.PropertyPath != Entry.PropertyPath
            && !OutResult.Diagnostic.PropertyPath.StartsWith(Entry.PropertyPath + TEXT(".")))
          OutResult.Diagnostic.PropertyPath = Entry.PropertyPath + TEXT(".") + OutResult.Diagnostic.PropertyPath;
        return false;
      }
      Deferred->SetTextIdentityPackage(Prepared.TextIdentityPackage);
      if (Entry.bDescendantsOnly) {
        Entry.CurrentValuePtr = Target.ValuePtr;
        ObjectProperty->SetObjectPropertyValue(Entry.DesiredValue->GetMutableValuePtr(), CurrentObject);
        Entry.CompletedValue = MakeObjectPathValue(CurrentObject);
      }
      if (Deferred->NumProperties() > 0)
        Entry.NestedOwnedPlans.Add(MoveTemp(Deferred));
      else {
        auto& Intent = Prepared.UnchangedOwnedIntents.AddDefaulted_GetRef();
        Intent.RelativePath = CurrentObject->GetPathName(Prepared.NotificationOwner);
        if (!Deferred->CaptureUnchangedIntent(Intent.Intent, Failure)) {
          OutResult.Diagnostic = Failure.Diagnostic; return false;
        }
      }
    }
    for (int32 NestedIndex = 0; NestedIndex < Entry.NestedOwnedPlans.Num();
         ++NestedIndex) {
      Entry.NestedPlansAttempted = NestedIndex + 1;
      FApplyResult NestedResult;
      if (!ApplyPrepared(*Entry.NestedOwnedPlans[NestedIndex], NestedResult)) {
        OutResult.Diagnostic = NestedResult.Diagnostic;
        OutResult.Diagnostic.PropertyPath = Entry.PropertyPath + TEXT(".") + OutResult.Diagnostic.PropertyPath;
        return false;
      }
    }
  }
  Prepared.bApplied = true;
  OutResult.Status = TEXT("applied");
  return true;
}

struct FVerificationRoot {
  UStruct* Struct = nullptr;
  void* Data = nullptr;
  UObject* Owner = nullptr;
  FString OwnerPath;
};

bool RefreshPropertyTarget(FPreparedApply::FImpl& Prepared,
                           FPreparedProperty& Entry, FApplyResult& Result,
                           const FVerificationRoot* VerificationRoot) {
  UeAgentAuthoringTargetLocator::FResolvedPropertyTarget Target;
  UeAgentAuthoringTargetLocator::FResolveResult ResolveResult;
  if (!UeAgentAuthoringTargetLocator::ResolvePropertyPath(
          VerificationRoot ? VerificationRoot->Struct : Prepared.RootStruct,
          VerificationRoot ? VerificationRoot->Data : Prepared.RootData,
          VerificationRoot ? VerificationRoot->OwnerPath : Prepared.RootOwnerPath,
          VerificationRoot ? VerificationRoot->Owner : Prepared.NotificationOwner,
          Entry.PropertyPath, Target, ResolveResult)) {
    Result.Diagnostic = ResolveResult.Diagnostic;
    if (Result.Diagnostic.Stage.IsEmpty()) Result.Diagnostic.Stage = TEXT("property_target_refresh");
    Result.Diagnostic.PropertyPath = Entry.PropertyPath;
    Result.Diagnostic.TargetPath = Prepared.Options.DiagnosticTargetPath.IsEmpty()
        ? Prepared.RootOwnerPath : Prepared.Options.DiagnosticTargetPath;
    return false;
  }
  if (!Entry.Property->SameType(Target.Property)) {
    Result.Diagnostic = UeAgentAuthoringDiagnostics::MakeValueFailure(
        TEXT("prepared_apply_target_type_changed"), Entry.PropertyPath,
        MakeShared<FJsonValueObject>(UeAgentPropertyValueCodec::BuildTypeDescriptor(Entry.Property)),
        MakeShared<FJsonValueObject>(UeAgentPropertyValueCodec::BuildTypeDescriptor(Target.Property)),
        TEXT("property_target_refresh"), TEXT("The property type changed between preparation and application."));
    Result.Diagnostic.SourceKind = TEXT("native_result");
    Result.Diagnostic.TargetPath = Prepared.Options.DiagnosticTargetPath.IsEmpty()
        ? Prepared.RootOwnerPath : Prepared.Options.DiagnosticTargetPath;
    return false;
  }
  Entry.CurrentValuePtr = Target.ValuePtr;
  Entry.NotificationOwner = Target.NotificationOwner;
  if (Prepared.Options.StructNotificationOwnerPropertyPath.IsEmpty())
    Entry.NotificationChain = MoveTemp(Target.NotificationChain);
  return true;
}

bool VerifyPreparedInternal(FPreparedApply::FImpl &Prepared,
                            FApplyResult &OutResult,
                            const FVerificationRoot* VerificationRoot) {
  ON_SCOPE_EXIT { SupplementLogicalTarget(OutResult.Diagnostic, Prepared.Options.DiagnosticTargetPath,
      VerificationRoot ? VerificationRoot->OwnerPath : Prepared.RootOwnerPath); };
  if (!Prepared.bApplied || Prepared.bRolledBack) {
    OutResult.Status = TEXT("failed");
    OutResult.Diagnostic = FDiagnostic::Failure(TEXT("prepared_apply_not_applied"), TEXT("The property plan has not reached the required applied state or has been rolled back."), TEXT("object_apply"));
    OutResult.Diagnostic.InternalContext = Prepared.RootOwnerPath;
    return false;
  }
  for (FPreparedProperty &Entry : Prepared.Properties) {
    // Notifications and domain work may reallocate a container or replace an
    // inline value. A prepared address is not evidence of the current value.
    if (!RefreshPropertyTarget(Prepared, Entry, OutResult, VerificationRoot)) {
      OutResult.Status = TEXT("failed");
      return false;
    }
    bool bEquivalent = false;
    FDiagnostic CompareError;
    TSharedPtr<FPropertyValueStorage> Expected;
    const void* ExpectedValue = Entry.DesiredValue->GetValuePtr();
    if (Entry.Node.ValueKind == TEXT("struct")) {
      Expected = FPropertyValueStorage::CopyFrom(Entry.Property, Entry.CurrentValuePtr, CompareError);
      if (Expected && CopyExplicitPropertyValue(Entry.Property, ExpectedValue,
          Expected->GetMutableValuePtr(), Entry.Node, CompareError)) ExpectedValue = Expected->GetValuePtr();
    }
    auto CompareOptions = MakeCodecOptions(Prepared.Options);
    CompareOptions.NativeMemberContainer = Entry.Property->GetOwner<UScriptStruct>() && Entry.Property->ArrayDim == 1
        ? static_cast<const uint8*>(Entry.CurrentValuePtr) - Entry.Property->GetOffset_ForInternal() : nullptr;
    const auto Compared = CompareError.IsEmpty()
        ? UeAgentPropertyValueCodec::ComparePropertyValues(Entry.Property, ExpectedValue,
            Entry.CurrentValuePtr, Entry.NotificationOwner, CompareOptions)
        : UeAgentPropertyValueCodec::FValueComparison();
    bEquivalent = CompareError.IsEmpty() && Compared.bEquivalent;
    if (!CompareError.IsEmpty() || !bEquivalent) {
      OutResult.Status = TEXT("failed");
      OutResult.Diagnostic =
          CompareError.IsEmpty() ? FDiagnostic::Failure(TEXT("readback_mismatch"), TEXT("The property readback differs from the requested value."), TEXT("object_apply")) : CompareError;
      OutResult.Diagnostic.InternalContext = Entry.PropertyPath;
      OutResult.Diagnostic.PropertyPath = Entry.PropertyPath;
      if (OutResult.Diagnostic.Stage.IsEmpty()) OutResult.Diagnostic.Stage = TEXT("property_intent_verify");
      OutResult.Diagnostic.PropertyPath = Entry.PropertyPath;
      OutResult.Diagnostic.TargetPath = !Prepared.Options.DiagnosticTargetPath.IsEmpty() ? Prepared.Options.DiagnosticTargetPath
          : Entry.NotificationOwner ? Entry.NotificationOwner->GetPathName() : Prepared.RootOwnerPath;
      if (CompareError.IsEmpty()) Compared.ApplyTo(OutResult.Diagnostic);
      return false;
    }
    for (const TUniquePtr<FPreparedApply> &NestedPlan :
         Entry.NestedOwnedPlans) {
      FApplyResult NestedResult;
      if (!VerifyPrepared(*NestedPlan, NestedResult)) {
        OutResult.Status = TEXT("failed");
        OutResult.Diagnostic = MoveTemp(NestedResult.Diagnostic);
        OutResult.Diagnostic.PropertyPath = Entry.PropertyPath + TEXT(".") + OutResult.Diagnostic.PropertyPath;
        return false;
      }
    }
  }
  OutResult.Status = TEXT("applied");
  OutResult.Diagnostic.Reset();
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
  ON_SCOPE_EXIT { SupplementLogicalTarget(OutResult.Diagnostic, Prepared.Options.DiagnosticTargetPath, Prepared.RootOwnerPath); };
  OutResult.bRollbackAttempted = true;
  if (!Prepared.Options.NativeStages.IsEmpty()) {
    OutResult.Status = TEXT("rollback_failed");
    OutResult.bRollbackSucceeded = false;
    OutResult.Diagnostic = FDiagnostic::Failure(TEXT("native_stages_checkpoint_restore_required"), TEXT("Native staged edits must be rolled back by restoring their domain checkpoint."), TEXT("object_apply"));
    return false;
  }
  bool bSucceeded = true;
  TMap<UObject*, FNotificationChains> DeferredNotifications;
  if (Prepared.bBatchNotifications && Prepared.Options.bEmitEditorNotifications) {
    for (auto& Entry : Prepared.Properties) {
      if (Entry.bPropertyApplied && !RefreshPropertyTarget(Prepared, Entry, OutResult)) {
        bSucceeded = false;
        continue;
      }
      if (Entry.bPropertyApplied && Entry.NotificationOwner)
        DeferredNotifications.FindOrAdd(Entry.NotificationOwner).Append(
            BuildNotifications(Entry, Entry.OriginalValue->GetValuePtr(), true, Prepared.Options));
    }
    for (const auto& Pair : DeferredNotifications) {
      if (Prepared.bWholeObjectNotification) Pair.Key->PreEditChange(static_cast<FProperty*>(nullptr));
      else NotifyPropertyChains(*Pair.Key, Pair.Value, true);
    }
  }
  for (int32 Index = Prepared.PropertiesAttempted - 1; Index >= 0; --Index) {
    FPreparedProperty &Entry = Prepared.Properties[Index];
    for (int32 NestedIndex = Entry.NestedPlansAttempted - 1; NestedIndex >= 0;
         --NestedIndex) {
      FApplyResult NestedResult;
      if (!RollbackPrepared(*Entry.NestedOwnedPlans[NestedIndex], NestedResult))
        bSucceeded = false;
    }
    if (Entry.bPropertyApplied) {
      if (!RefreshPropertyTarget(Prepared, Entry, OutResult)) {
        bSucceeded = false;
        CleanupCreatedObjects(Entry);
        continue;
      }
      UObject *Owner = Entry.NotificationOwner;
      const auto Notifications = BuildNotifications(Entry, Entry.OriginalValue->GetValuePtr(), Prepared.Options.bEmitEditorNotifications, Prepared.Options);
      if (Owner && !Prepared.bBatchNotifications) NotifyPropertyChains(*Owner, Notifications, true);
      if (Owner && Prepared.Options.NativePropertyTransition)
        Prepared.Options.NativePropertyTransition(*Owner, *Entry.Property, Entry.Node, true);
      Entry.Property->CopyCompleteValue(Entry.CurrentValuePtr,
                                        Entry.OriginalValue->GetValuePtr());
      if (Entry.LinkedTraceBefore)
        if (auto* Trace = UeAgentEnvTraceDataValue::LinkedMemberContainer(Entry.Property, Entry.CurrentValuePtr))
          UeAgentEnvTraceDataValue::CopyChannels(*Entry.LinkedTraceBefore, *Trace);
      if (Owner && !Prepared.bBatchNotifications) {
        FFailure Failure;
        if (!NotifyPropertyChains(*Owner, Notifications, false, Prepared.Options.NotificationChangeType,
            Prepared.Options.PostEditNotification, Prepared.Options.CheckedPostEditNotification, &Failure)) {
          bSucceeded = false;
          SetNotificationFailure(OutResult, Failure);
        }
      }
      ++OutResult.PropertiesRolledBack;
    }
    CleanupCreatedObjects(Entry);
  }
  // Restoring a dependent field before its mesh and immediately notifying can
  // destroy the restored value just as it can during forward application.
  for (const auto& Pair : DeferredNotifications) {
    FFailure Failure;
    if (!NotifyPropertyBatch(*Pair.Key, Pair.Value, Prepared.Options, Prepared.bWholeObjectNotification, Failure)) {
      bSucceeded = false;
      SetNotificationFailure(OutResult, Failure);
    }
  }
  for (const auto& Pair : Prepared.SplineEditedBefore) {
    Pair.Key->SetSpline(Pair.Key->GetSplineCurves());
    Pair.Key->SetOverrideConstructionScript(Pair.Value);
    Pair.Key->UpdateSpline();
  }
  FDiagnostic InstanceRestoreError;
  if (!Prepared.InheritedInstanceValues.Restore(InstanceRestoreError)) {
    bSucceeded = false;
    OutResult.Diagnostic = InstanceRestoreError;
  }
  for (const TPair<UPackage *, bool> &Pair : Prepared.PackageDirtyStates) {
    if (Pair.Key) {
      if (const auto* Metadata = Prepared.TextMetadataBefore.Find(Pair.Key)) {
        for (const auto& Entry : *Metadata) {
          if (Entry.Value.IsSet()) Pair.Key->GetMetaData().RootMetaDataMap.Add(Entry.Key, Entry.Value.GetValue());
          else Pair.Key->GetMetaData().RootMetaDataMap.Remove(Entry.Key);
        }
      }
    }
    if (Pair.Key)
      Pair.Key->SetDirtyFlag(Pair.Value);
  }
  Prepared.bRolledBack = true;
  Prepared.bApplied = false;
  OutResult.Status = bSucceeded ? TEXT("rolled_back") : TEXT("rollback_failed");
  OutResult.bRollbackSucceeded = bSucceeded;
  if (!bSucceeded && OutResult.Diagnostic.Code.IsEmpty())
    OutResult.Diagnostic = FDiagnostic::Failure(TEXT("object_apply_rollback_failed"), TEXT("At least one prepared property could not be restored during rollback."), TEXT("object_apply"));
  return bSucceeded;
}
} // namespace

namespace UeAgentAuthoringObjectApply {

void ApplyStructChanges(UScriptStruct* Struct, const void* Before,
    const void* Edited, void* Current, UObject* Owner) {
  const FStructChange Edit{Before, Edited};
  FDiagnostic Error;
  const bool bApplied = TryApplyStructChanges(Struct, MakeArrayView(&Edit, 1), Current, Owner, Error);
  check(bApplied);
}

bool TryApplyStructChanges(UScriptStruct* Struct, TConstArrayView<FStructChange> Edits,
    void* Current, UObject* Owner, FDiagnostic& Error) {
  check(Struct && Current);
  Error.Reset();
  struct FFieldEdit { TArray<FProperty*> Chain; const void* Value; };
  TArray<FFieldEdit> Fields;
  UeAgentPropertyValueCodec::FOptions Options;
  Options.bCompareStoredValues = true;
  for (const FStructChange& Edit : Edits) {
    check(Edit.Before && Edit.Edited);
    FNotificationChains Changes;
    for (TFieldIterator<FProperty> It(Struct); It; ++It) {
      if (It->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated)) continue;
      TArray<FProperty*> Chain{*It};
      GatherChangedNotifications(*It, It->ContainerPtrToValuePtr<void>(Edit.Before),
          It->ContainerPtrToValuePtr<void>(Edit.Edited), Chain, Changes, Owner, Options);
    }
    for (auto& Chain : Changes) {
      const void* Value = Edit.Edited;
      for (FProperty* Field : Chain) Value = Field->ContainerPtrToValuePtr<void>(Value);
      const FFieldEdit* Existing = Fields.FindByPredicate([&](const FFieldEdit& Field) { return Field.Chain == Chain; });
      if (Existing) {
        const auto Compared = UeAgentPropertyValueCodec::ComparePropertyValues(Chain.Last(), Existing->Value, Value, Owner, Options);
        if (!Compared.bEquivalent) {
          FString Path;
          for (FProperty* Field : Chain) { if (!Path.IsEmpty()) Path += TEXT("."); Path += Field->GetName(); }
          Error = FDiagnostic::Failure(TEXT("object_apply_conflicting_struct_edits"), TEXT("Independent edits conflict at the same struct field."), TEXT("object_apply"));
          Error.PropertyPath = Path;
          Compared.ApplyTo(Error);
          return false;
        }
      } else Fields.Add({MoveTemp(Chain), Value});
    }
  }
  for (const FFieldEdit& Edit : Fields) {
    void* Destination = Current;
    for (FProperty* Field : Edit.Chain) {
      Destination = Field->ContainerPtrToValuePtr<void>(Destination);
    }
    Edit.Chain.Last()->CopyCompleteValue(Destination, Edit.Value);
  }
  return true;
}
bool NotifyObject(UObject& Owner, const FOptions& Options, FFailure& OutFailure) {
  OutFailure = {};
  return NotifyWholeObject(Owner, Options, OutFailure, true);
}

bool NotifyObjectMembers(UObject& Owner, const TArray<FName>& Members,
                         bool bPre, FFailure& OutFailure) {
  FNotificationChains Notifications;
  for (const FName Member : Members) {
    FProperty* Property = FindFProperty<FProperty>(Owner.GetClass(), Member);
    if (!Property) {
      OutFailure.Diagnostic = FDiagnostic::Failure(TEXT("notification_owner_member_missing"), TEXT("The requested notification member does not exist on the native owner."), TEXT("object_apply"));
      OutFailure.Diagnostic.PropertyPath = Member.ToString();
      return false;
    }
    Notifications.Add({Property});
  }
  NotifyPropertyChains(Owner, Notifications, bPre);
  return true;
}

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
  Object->SetStringField(TEXT("kind"), ValueKind);
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
  return Status == TEXT("applied") && Diagnostic.IsEmpty();
}

TSharedRef<FJsonObject> FApplyResult::ToJsonObject() const {
  TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
  Object->SetStringField(TEXT("schema"), TEXT("uai.object_apply_result.v1"));
  Object->SetStringField(TEXT("status"), Status);
  if (!Diagnostic.IsEmpty()) Object->SetObjectField(TEXT("diagnostic"), Diagnostic.ToPublicJson());
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

bool FPreparedApply::HasTextSourceEdits() const {
  if (!Impl) return false;
  for (const auto& Entry : Impl->Properties) {
    if (!Entry.TextSourceEdits.IsEmpty()) return true;
    for (const auto& Nested : Entry.NestedOwnedPlans) if (Nested->HasTextSourceEdits()) return true;
    for (const auto& Materialization : Entry.Materializations) if (Materialization.bHasTextSourceEdits) return true;
  }
  return false;
}

void FPreparedApply::SetTextIdentityPackage(UPackage* Package) {
  if (!Impl) return;
  Impl->TextIdentityPackage = Package;
  for (auto& Entry : Impl->Properties)
    for (auto& Nested : Entry.NestedOwnedPlans) Nested->SetTextIdentityPackage(Package);
}

TArray<TSharedPtr<FJsonValue>> FPreparedApply::GetResolvedPropertyDescriptors() const {
  TMap<const FJsonValue*, TSharedPtr<FJsonValue>> Replacements;
  TFunction<void(const FPreparedApply&)> Collect = [&](const FPreparedApply& Plan) {
    if (!Plan.Impl) return;
    for (const auto& Entry : Plan.Impl->Properties) {
      for (const auto& Edit : Entry.TextSourceEdits)
        Replacements.Add(Edit.Input.Get(), MakeShared<FJsonValueObject>(Edit.Completed));
      for (const auto& Nested : Entry.NestedOwnedPlans) Collect(*Nested);
    }
  };
  Collect(*this);
  TFunction<TSharedPtr<FJsonValue>(const TSharedPtr<FJsonValue>&)> Copy = [&](const TSharedPtr<FJsonValue>& Value) -> TSharedPtr<FJsonValue> {
    if (const auto* Resolved = Replacements.Find(Value.Get()))
      return UeAgentAuthoringState::CloneCanonicalValue(*Resolved);
    if (Value.IsValid() && Value->Type == EJson::Object) {
      auto Object = MakeShared<FJsonObject>();
      for (const auto& Field : Value->AsObject()->Values) Object->SetField(Field.Key, Copy(Field.Value));
      return MakeShared<FJsonValueObject>(Object);
    }
    if (Value.IsValid() && Value->Type == EJson::Array) {
      TArray<TSharedPtr<FJsonValue>> Items;
      for (const auto& Item : Value->AsArray()) Items.Add(Copy(Item));
      return MakeShared<FJsonValueArray>(Items);
    }
    return UeAgentAuthoringState::CloneCanonicalValue(Value);
  };
  TArray<TSharedPtr<FJsonValue>> Result;
  if (Impl) for (const auto& Descriptor : Impl->SourceDescriptors) Result.Add(Copy(Descriptor));
  return Result;
}

const TArray<FResolvedNode> &FPreparedApply::GetResolvedNodes() const {
  static const TArray<FResolvedNode> Empty;
  return Impl.IsValid() ? Impl->Nodes : Empty;
}

bool FPreparedApply::CaptureIntent(FPropertyIntent& OutIntent, FFailure& OutFailure, bool bIncludeUnchanged) const {
  return CaptureIntentInternal(OutIntent, OutFailure, false, bIncludeUnchanged);
}

bool FPreparedApply::CaptureUnchangedIntent(FPropertyIntent& OutIntent, FFailure& OutFailure) const {
  return CaptureIntentInternal(OutIntent, OutFailure, true, true);
}

bool FPreparedApply::CaptureIntentInternal(FPropertyIntent& OutIntent, FFailure& OutFailure,
    bool bUnchangedOnly, bool bIncludeUnchanged) const {
  OutIntent = FPropertyIntent();
  OutFailure = FFailure();
  if (!Impl || !Impl->bReady || (!bUnchangedOnly && !Impl->bApplied) || Impl->bRolledBack) {
    OutFailure.Diagnostic = FDiagnostic::Failure(TEXT("prepared_apply_not_applied"), TEXT("The property plan has not reached the required applied state or has been rolled back."), TEXT("object_apply"));
    if (Impl) OutFailure.Diagnostic.TargetPath = Impl->Options.DiagnosticTargetPath;
    return false;
  }
  auto Frozen = MakeShared<FPropertyIntent::FImpl>();
  Frozen->CapturedOwnerPath = Impl->NotificationOwner ? Impl->NotificationOwner->GetPathName() : FString();
  Frozen->DiagnosticTargetPath = Impl->Options.DiagnosticTargetPath;
  Frozen->bAllowReadOnly = Impl->Options.bAllowReadOnly;
  if (bIncludeUnchanged) {
    Frozen->Values = Impl->UnchangedValues;
    Frozen->Owned = Impl->UnchangedOwnedIntents;
  }
  const auto Codec = MakeCodecOptions(Impl->Options);
  for (const auto& Entry : Impl->Properties) {
    if (bUnchangedOnly) break;
    auto& Value = Frozen->Values.AddDefaulted_GetRef();
    Value.Path = Entry.PropertyPath;
    Value.Type = UeAgentPropertyValueCodec::BuildTypeDescriptor(Entry.Property);
    Value.Value = ProjectExplicitIntentValue(Entry.Node,
        UeAgentPropertyValueCodec::Encode(Entry.Property,
            Entry.DesiredValue->GetValuePtr(), Entry.NotificationOwner, Codec));
    if (!Value.Type || !Value.Value) {
      OutFailure.Diagnostic = FDiagnostic::Failure(TEXT("property_intent_capture_failed"), TEXT("The requested property type or value could not be captured for verification."), TEXT("object_apply"));
      OutFailure.Diagnostic.PropertyPath = Entry.PropertyPath; return false;
    }
    for (const auto& Descriptor : Impl->SourceDescriptors) {
      FString Path;
      if (Descriptor && Descriptor->Type == EJson::Object
          && Descriptor->AsObject()->TryGetStringField(TEXT("property_path"), Path) && Path == Value.Path) {
        const TSharedPtr<FJsonObject>* Source = nullptr;
        if (Descriptor->AsObject()->TryGetObjectField(TEXT("authoring_source"), Source))
          Value.Source = UeAgentAuthoringState::CloneCanonicalValue(MakeShared<FJsonValueObject>(*Source))->AsObject();
        break;
      }
    }
    for (const auto& Nested : Entry.NestedOwnedPlans) {
      UObject* NestedRoot = Nested->Impl ? Nested->Impl->RootObject : nullptr;
      if (!NestedRoot || !Impl->NotificationOwner || !NestedRoot->IsIn(Impl->NotificationOwner)) {
        OutFailure.Diagnostic = FDiagnostic::Failure(TEXT("property_intent_owned_root_invalid"), TEXT("The nested property intent is no longer owned by the captured root."), TEXT("object_apply"));
        OutFailure.Diagnostic.PropertyPath = Entry.PropertyPath; return false;
      }
      auto& Owned = Frozen->Owned.AddDefaulted_GetRef();
      Owned.RelativePath = NestedRoot->GetPathName(Impl->NotificationOwner);
      if (!Nested->CaptureIntent(Owned.Intent, OutFailure)) return false;
    }
  }
  OutIntent.Impl = MoveTemp(Frozen);
  return true;
}

bool FPropertyIntent::IsEmpty() const {
  return !Impl || (Impl->Values.IsEmpty() && Impl->Owned.IsEmpty());
}

bool FPropertyIntent::VerifyObject(UObject* RootObject, FApplyResult& OutResult) const {
  return VerifyStruct(RootObject ? RootObject->GetClass() : nullptr, RootObject, RootObject, OutResult);
}

bool FPropertyIntent::VerifyStruct(UStruct* RootStruct, void* RootData, UObject* Owner,
                                   FApplyResult& OutResult) const {
  ON_SCOPE_EXIT { if (Impl) SupplementLogicalTarget(OutResult.Diagnostic, Impl->DiagnosticTargetPath,
      Owner ? Owner->GetPathName() : RootStruct ? RootStruct->GetPathName() : FString()); };
  OutResult = FApplyResult();
  OutResult.Status = TEXT("failed");
  if (!Impl || !RootStruct || !RootData) {
    OutResult.Diagnostic = FDiagnostic::Failure(TEXT("property_intent_root_missing"), TEXT("The property intent requires a live root type and data."), TEXT("object_apply")); return false;
  }
  FOptions Options;
  Options.bAllowReadOnly = Impl->bAllowReadOnly;
  auto Codec = MakeCodecOptions(Options);
  Codec.bAllowPartialStructFields = true;
  // Only references are rebound; string values that happen to contain a path
  // remain exact strings. Domain callers have already resolved the current root.
  Codec.ScopedReferenceDecoder = [&](const TSharedPtr<FJsonValue>& Input,
      TSharedPtr<FJsonValue>& Output, FDiagnostic& Error) {
    TOptional<FString> Path;
    if (!UeAgentPropertyValueCodec::DecodeReferencePath(Input, Path, Error)) return false;
    if (Path.IsSet() && Owner && !Impl->CapturedOwnerPath.IsEmpty()) {
      const auto& Before = Impl->CapturedOwnerPath;
      if (Path.GetValue() == Before) {
        Path = Owner->GetPathName();
      } else if (Path.GetValue().StartsWith(Before + TEXT("."))
          || Path.GetValue().StartsWith(Before + FString::Chr(SUBOBJECT_DELIMITER_CHAR))) {
        // GetPathName separates the first subobject with ':' and deeper
        // descendants with '.'. Publication can change the root's outer depth.
        const TCHAR Separator = Owner->GetOuter() && Owner->GetOuter()->IsA<UPackage>()
            ? SUBOBJECT_DELIMITER_CHAR : TEXT('.');
        Path = Owner->GetPathName() + FString::Chr(Separator) + Path.GetValue().Mid(Before.Len() + 1);
      }
    }
    Output = UeAgentPropertyValueCodec::EncodeReferencePath(Path.IsSet() ? Path.GetValue() : FString());
    return true;
  };
  for (const auto& Value : Impl->Values) {
    UeAgentAuthoringTargetLocator::FResolvedPropertyTarget Target;
    UeAgentAuthoringTargetLocator::FResolveResult Resolve;
    FDiagnostic Error;
    auto Fail = [&](FDiagnostic Failure) {
      OutResult.Diagnostic = MoveTemp(Failure);
      OutResult.Diagnostic.InternalContext = Value.Path;
      OutResult.Diagnostic.PropertyPath = Value.Path;
      if (OutResult.Diagnostic.Stage.IsEmpty()) OutResult.Diagnostic.Stage = TEXT("property_intent_verify");
      auto& Diagnostic = OutResult.Diagnostic;
      Diagnostic.PropertyPath = Value.Path;
      if (Value.Source) {
        FJsonObject Descriptor;
        Descriptor.SetObjectField(TEXT("authoring_source"), Value.Source);
        UeAgentAuthoringDiagnostics::LocatePropertyValueSource(Diagnostic, Descriptor);
      }
      if (Diagnostic.TargetPath.IsEmpty()) Diagnostic.TargetPath = !Impl->DiagnosticTargetPath.IsEmpty()
          ? Impl->DiagnosticTargetPath : Owner ? Owner->GetPathName() : RootStruct->GetPathName();
      return false;
    };
    if (!UeAgentAuthoringTargetLocator::ResolvePropertyPath(RootStruct, RootData,
        Owner ? Owner->GetPathName() : RootStruct->GetPathName(), Owner, Value.Path, Target, Resolve))
      return Fail(Resolve.Diagnostic);
    if (!UeAgentPropertyValueCodec::ValidateTypeMetadata(Target.Property, Value.Type, Error))
      return Fail(Error);
    Codec.NativeMemberContainer = Target.Property->GetOwner<UScriptStruct>() && Target.Property->ArrayDim == 1
        ? static_cast<const uint8*>(Target.ValuePtr) - Target.Property->GetOffset_ForInternal() : nullptr;
    if (!UeAgentPropertyValueCodec::VerifyValueIntent(Target.Property, Value.Value,
        Target.ValuePtr, Target.NotificationOwner, Codec, Error)) return Fail(Error);
  }
  for (const auto& Owned : Impl->Owned) {
    UObject* Current = Owner ? StaticFindObject(UObject::StaticClass(), Owner, *Owned.RelativePath) : nullptr;
    if (!Owned.Intent.VerifyObject(Current, OutResult)) return false;
  }
  OutResult.Status = TEXT("applied");
  OutResult.PropertiesPrepared = Impl->Values.Num();
  return true;
}

bool FPreparedApply::CaptureNativeNormalizedDescriptors(
    TArray<TSharedPtr<FJsonValue>>& OutDescriptors, FFailure& OutFailure) const {
  OutFailure = FFailure();
  if (!Impl || !Impl->bApplied || Impl->bRolledBack) {
    OutFailure.Diagnostic = FDiagnostic::Failure(TEXT("native_normalization_preview_not_applied"), TEXT("Native normalization cannot be captured before the preview plan is applied."), TEXT("object_apply")); return false;
  }
  OutDescriptors = GetResolvedPropertyDescriptors();
  for (const auto& Entry : Impl->Properties) {
    UeAgentAuthoringTargetLocator::FResolvedPropertyTarget Target;
    UeAgentAuthoringTargetLocator::FResolveResult ResolveResult;
    if (!UeAgentAuthoringTargetLocator::ResolvePropertyPath(Impl->RootStruct,
        Impl->RootData, Impl->RootOwnerPath, Impl->NotificationOwner,
        Entry.PropertyPath, Target, ResolveResult)) {
      OutFailure.Diagnostic = FDiagnostic::Failure(TEXT("native_normalization_target_lost"), TEXT("The property target is unavailable after native normalization."), TEXT("object_apply"));
      OutFailure.Diagnostic.PropertyPath = Entry.PropertyPath; return false;
    }
    bool bEquivalent = false; FDiagnostic Error;
    if (!UeAgentAuthoringSemanticCompare::ArePropertyValuesEquivalent(Target.Property,
        Target.ValuePtr, Entry.DesiredValue->GetValuePtr(), Target.NotificationOwner,
        bEquivalent, Error)) {
      OutFailure.Diagnostic = Error; OutFailure.Diagnostic.PropertyPath = Entry.PropertyPath; return false;
    }
    if (bEquivalent) continue;
    // A native ownership change needs its own identity contract. Do not turn
    // it into a new anonymous candidate by recapturing an entire owned tree.
    if (!Entry.Materializations.IsEmpty() || !Entry.NestedOwnedPlans.IsEmpty()
        || !Entry.ExistingInlinePatches.IsEmpty()) {
      OutFailure.Diagnostic = FDiagnostic::Failure(TEXT("native_normalization_owned_identity_changed"), TEXT("Native normalization replaced an owned object required by the prepared edit."), TEXT("object_apply"));
      OutFailure.Diagnostic.PropertyPath = Entry.PropertyPath; return false;
    }
    auto CodecOptions = MakeCodecOptions(Impl->Options);
    CodecOptions.bCaptureStructFieldDescriptors = true;
    if (!Impl->Options.bAllowReadOnly) {
      CodecOptions.StructFieldFilter = [](const UScriptStruct* Struct, const FProperty* Field) {
        return UeAgentPropertyValueCodec::IsStructFieldAuthoringEditable(Field, Struct);
      };
    }
    TSharedPtr<FJsonObject> Definition;
    auto Value = UeAgentPropertyValueCodec::Encode(Target.Property, Target.ValuePtr,
        Target.NotificationOwner, CodecOptions, 0, &Definition);
    if (!Value.IsValid()) {
      OutFailure.Diagnostic = FDiagnostic::Failure(TEXT("native_normalization_capture_failed"), TEXT("The native normalized property value could not be captured."), TEXT("object_apply"));
      OutFailure.Diagnostic.PropertyPath = Entry.PropertyPath; return false;
    }
    // This value returns to an authoring input, not to the complete readback
    // snapshot. Reuse the normal projection so native-only struct caches do
    // not become explicit writes on the next prepare.
    if (!Impl->Options.bAllowReadOnly) {
      TSharedPtr<FJsonValue> ProjectedValue, ProjectedDefinition;
      TSharedPtr<FJsonValue> AuthoringDefinition = Definition
          ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueObject>(Definition)) : nullptr;
      for (const auto& Source : OutDescriptors) {
        FString SourcePath;
        if (Source.IsValid() && Source->Type == EJson::Object
            && Source->AsObject()->TryGetStringField(TEXT("property_path"), SourcePath)
            && SourcePath == Entry.PropertyPath) {
          if (auto OriginalDefinition = Source->AsObject()->TryGetField(TEXT("value_definition")))
            AuthoringDefinition = OriginalDefinition;
          break;
        }
      }
      if (!UeAgentPropertyValueCodec::ProjectAuthoringValue(
              UeAgentPropertyValueCodec::BuildTypeDescriptor(Target.Property), Value,
              AuthoringDefinition,
              ProjectedValue, ProjectedDefinition)) {
        OutFailure.Diagnostic = FDiagnostic::Failure(TEXT("native_normalization_capture_failed"), TEXT("The native normalized property value could not be captured."), TEXT("object_apply"));
        OutFailure.Diagnostic.PropertyPath = Entry.PropertyPath; return false;
      }
      Value = ProjectedValue;
    }
    Value = ProjectNativeNormalizedIntentValue(Entry.Node, Target.Property,
        Entry.DesiredValue->GetValuePtr(), Target.ValuePtr, Target.NotificationOwner, Value, Error);
    if (!Value.IsValid()) {
      OutFailure.Diagnostic = Error.IsEmpty() ? FDiagnostic::Failure(TEXT("native_normalization_capture_failed"), TEXT("The native normalized property value could not be captured."), TEXT("object_apply")) : Error;
      OutFailure.Diagnostic.PropertyPath = Entry.PropertyPath; return false;
    }
    for (auto& DescriptorValue : OutDescriptors) {
      if (!DescriptorValue.IsValid() || DescriptorValue->Type != EJson::Object) continue;
      auto Descriptor = DescriptorValue->AsObject(); FString Path;
      if (Descriptor->TryGetStringField(TEXT("property_path"), Path) && Path == Entry.PropertyPath) {
        if (UeAgentNativeNormalizationIntent::IsRecording()) {
          // Evidence is rebound through the authoring capture after native
          // normalization. Use the same codec field scope on both sides; a
          // native-only cache is neither an authored edit nor an input field.
          const auto SourceValue = UeAgentPropertyValueCodec::Encode(Target.Property,
              Entry.DesiredValue->GetValuePtr(), Target.NotificationOwner, CodecOptions);
          if (!UeAgentNativeNormalizationIntent::Record(Target.Identity,
              SourceValue, Descriptor->TryGetField(TEXT("value")), Value, Error)) {
            if (Error.PropertyPath.IsEmpty()) Error.PropertyPath = Entry.PropertyPath;
            UeAgentAuthoringDiagnostics::ReportConsistencyWarning(FString(), Error, TEXT("native_normalization_observation"));
          }
        }
        Descriptor->SetField(TEXT("value"), Value);
      }
    }
  }
  return true;
}

bool PrepareObject(UObject *RootObject,
                   const TArray<TSharedPtr<FJsonValue>> &PropertyDescriptors,
                   const FOptions &Options, FPreparedApply &OutPrepared,
                   FFailure &OutFailure) {
  FPrepareSession Session;
  FOptions NativeOptions = Options;
  if (RootObject && Options.bEmitEditorNotifications && !Options.bWholeObjectNotification) {
    const auto Selectors = UeAgentBlackboardKeySelectorValue::RootProperties(*RootObject);
    if (!Selectors.IsEmpty()) {
      // Reuse native stages so dependent descriptors are decoded after key
      // notifications, rather than overwriting the selector's native effects.
      FOptions::FNativeStage Keys;
      Keys.RootProperties = Selectors;
      for (auto& Stage : NativeOptions.NativeStages)
        for (FName Name : Selectors) Stage.RootProperties.Remove(Name);
      NativeOptions.NativeStages.Insert(MoveTemp(Keys), 0);
    }
  }
  return PrepareRootInternal(
      RootObject ? RootObject->GetClass() : nullptr, RootObject, RootObject,
      RootObject, RootObject ? RootObject->GetPathName() : FString(),
      PropertyDescriptors, NativeOptions, Session, OutPrepared, OutFailure);
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
  if (!Prepared.Impl.IsValid() || !ApplyPreparedInternal(*Prepared.Impl, OutResult)) return false;
  ObservePrepared(Prepared);
  if (UeAgentNativeNormalizationIntent::IsActive()) {
    FPropertyIntent Intent;
    FFailure Failure;
    if (Prepared.CaptureIntent(Intent, Failure))
      UeAgentNativeNormalizationIntent::RecordDecodedProperties(Prepared.Impl->RootOwnerPath,
          Prepared.Impl->RootStruct->GetPathName(), Prepared.Impl->SourceDescriptors, Intent);
    else
      UeAgentAuthoringDiagnostics::ReportConsistencyWarning(Failure.Diagnostic.Code,
          Failure.Diagnostic, TEXT("property_intent_capture"));
  }
  return true;
}

void ObservePrepared(FPreparedApply& Prepared) {
  FApplyResult Observed;
  if (!VerifyPrepared(Prepared, Observed))
    UeAgentAuthoringDiagnostics::ReportConsistencyWarning(Observed.Diagnostic.Code,
        Observed.Diagnostic, TEXT("property_observation"));
}

static bool VerifyUnchangedIntent(FPreparedApply& Prepared, UStruct* RootStruct,
    void* RootData, UObject* Owner, FApplyResult& OutResult) {
  FPropertyIntent Intent;
  FFailure Failure;
  if (!Prepared.CaptureUnchangedIntent(Intent, Failure)) {
    OutResult.Status = TEXT("failed"); OutResult.Diagnostic = Failure.Diagnostic; return false;
  }
  if (Intent.IsEmpty()) return true;
  FApplyResult Verified;
  if (Intent.VerifyStruct(RootStruct, RootData, Owner, Verified)) return true;
  OutResult = MoveTemp(Verified);
  return false;
}

bool FPreparedApply::ApplyNativeNormalizationPreview(FApplyResult& OutResult) {
  if (!Impl || !ApplyPreparedInternal(*Impl, OutResult)) return false;
  FApplyResult Unchanged;
  if (!VerifyUnchangedIntent(*this, Impl->RootStruct, Impl->RootData,
      Impl->NotificationOwner, Unchanged))
    UeAgentAuthoringDiagnostics::ReportConsistencyWarning(Unchanged.Diagnostic.Code,
        Unchanged.Diagnostic, TEXT("native_normalization_preservation"));
  return true;
}

bool VerifyPrepared(FPreparedApply &Prepared, FApplyResult &OutResult) {
  return Prepared.Impl.IsValid() && VerifyPreparedInternal(*Prepared.Impl, OutResult)
      && VerifyUnchangedIntent(Prepared, Prepared.Impl->RootStruct, Prepared.Impl->RootData,
          Prepared.Impl->NotificationOwner, OutResult);
}

bool VerifyPreparedStruct(FPreparedApply &Prepared, UStruct* RootStruct,
                          void* RootData, UObject* NotificationOwner,
                          FApplyResult &OutResult) {
  OutResult = FApplyResult();
  if (!Prepared.Impl.IsValid() || Prepared.Impl->RootObject != nullptr
      || !RootStruct || RootStruct != Prepared.Impl->RootStruct || !RootData) {
    OutResult.Status = TEXT("failed");
    OutResult.Diagnostic = FDiagnostic::Failure(TEXT("prepared_apply_verification_root_invalid"), TEXT("Verification requires a live native root type and data."), TEXT("object_apply"));
    if (Prepared.Impl.IsValid()) OutResult.Diagnostic.TargetPath = Prepared.Impl->Options.DiagnosticTargetPath;
    return false;
  }
  const FVerificationRoot Root{RootStruct, RootData, NotificationOwner,
      NotificationOwner ? NotificationOwner->GetPathName() : RootStruct->GetPathName()};
  return VerifyPreparedInternal(*Prepared.Impl, OutResult, &Root)
      && VerifyUnchangedIntent(Prepared, RootStruct, RootData, NotificationOwner, OutResult);
}

bool RollbackPrepared(FPreparedApply &Prepared, FApplyResult &OutResult) {
  if (!Prepared.Impl.IsValid()) {
    OutResult = FApplyResult();
    OutResult.Status = TEXT("rollback_failed");
    OutResult.Diagnostic = FDiagnostic::Failure(TEXT("prepared_apply_missing"), TEXT("No prepared property plan is available to apply."), TEXT("object_apply"));
    return false;
  }
  return RollbackPreparedInternal(*Prepared.Impl, OutResult);
}
} // namespace UeAgentAuthoringObjectApply
