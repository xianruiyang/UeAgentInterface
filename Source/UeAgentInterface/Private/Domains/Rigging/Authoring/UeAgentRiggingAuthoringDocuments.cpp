// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Rigging/Authoring/UeAgentRiggingAuthoringDocuments.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Utilities/UeAgentReadableName.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTypeDescriptors.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringEncodedTypes.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringJsonFields.h"
#include "Domains/Rigging/Authoring/UeAgentRigVMNodeAuthoring.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAssetLifecycle.h"

#include "ControlRigBlueprint.h"
#include "ControlRigBlueprintFactory.h"
#include "Core/HjsonWorkspace/UeAgentGraphAuthoringProjection.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Domains/Rigging/UeAgentControlRigDomainBackend.h"
#include "Domains/Rigging/UeAgentIKDomainBackend.h"
#include "Engine/SkeletalMesh.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "QueryOwners/AnimationRigGraph/UeAgentAnimationRigGraphCandidateQuery.h"
#include "RetargetEditor/IKRetargetFactory.h"
#include "RetargetEditor/IKRetargeterController.h"
#include "Retargeter/IKRetargetChainMapping.h"
#include "Retargeter/IKRetargeter.h"
#include "Rig/IKRigDefinition.h"
#include "Rig/Solvers/IKRigSolverBase.h"
#include "RigEditor/IKRigController.h"
#include "RigEditor/IKRigDefinitionFactory.h"
#include "RigVMModel/Nodes/RigVMTemplateNode.h"
#include "RigVMModel/Nodes/RigVMUnitNode.h"
#include "RigVMModel/Nodes/RigVMVariableNode.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMLink.h"
#include "RigVMModel/RigVMNode.h"
#include "RigVMModel/RigVMPin.h"
#include "Rigs/RigHierarchy.h"
#include "Rigs/RigHierarchyController.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringObjectApply/UeAgentAuthoringObjectApply.h"
#include "Utilities/UeAgentDetailsAuthoring.h"
#include "Utilities/UeAgentPropertyValueCodec.h"

namespace UeAgentRiggingAuthoring {
namespace {
using FCodecOptions = UeAgentPropertyValueCodec::FOptions;

TSharedPtr<FJsonValue> EncodeStruct(UScriptStruct *Struct, const void *Value,
                                    UObject *Owner = nullptr) {
  return UeAgentPropertyValueCodec::EncodeStructValue(Struct, Value, Owner,
                                                      FCodecOptions());
}

TSharedPtr<FJsonValue>
EncodeNativeStorageDeltaFromDefault(UScriptStruct* Struct, const void* Value, UObject* Owner,
    const TSet<FName>& SeparateFields = {}) {
  if (Struct && Value) {
    FStructOnScope NativeDefault(Struct);
    // Solver/op containers own non-Edit UPROPERTY settings. Preserve their
    // complete persisted codec state independently of Details eligibility.
    FCodecOptions Options;
    Options.StructFieldFilter = [Struct, &SeparateFields](const UScriptStruct* FieldStruct, const FProperty* Field) {
      return FieldStruct != Struct || !SeparateFields.Contains(Field->GetFName());
    };
    const auto Encoded = UeAgentPropertyValueCodec::EncodeStructValue(Struct, Value, Owner, Options);
    const TSharedPtr<FJsonObject>* NativeFields = nullptr;
    if (Encoded.IsValid() && Encoded->Type == EJson::Object
        && Encoded->AsObject()->TryGetObjectField(TEXT("fields"), NativeFields)) {
      const auto Projected = UeAgentDetailsAuthoring::CloneObjectTree(Encoded->AsObject());
      Projected->SetObjectField(TEXT("fields"), UeAgentPropertyValueCodec::ProjectNativeDefaultStructFields(
          Struct, Value, NativeDefault.GetStructMemory(), *NativeFields));
      const TSharedPtr<FJsonValue> ProjectedValue = MakeShared<FJsonValueObject>(Projected);
      UeAgentAuthoringEncodedTypes::FScope::Copy(Encoded, ProjectedValue);
      return ProjectedValue;
    }
    return Encoded;
  }
  auto Result = MakeShared<FJsonObject>();
  Result->SetObjectField(TEXT("fields"), MakeShared<FJsonObject>());
  return MakeShared<FJsonValueObject>(Result);
}

bool TryCaptureSettingsDelta(UScriptStruct* Struct, const void* Value, UObject* Owner,
    const FString& SemanticPointer, TSharedPtr<FJsonValue>& OutValue, UE::UAI::Contract::FDiagnostic& OutError) {
  OutValue.Reset();
  TSharedPtr<FJsonObject> Fields = MakeShared<FJsonObject>();
  if (Struct && Value) {
    FStructOnScope NativeDefault(Struct);
    const FString RootOwnerPath = Owner ? Owner->GetPathName() : FString::Printf(TEXT("struct:%s"), *Struct->GetPathName());
    if (!UeAgentDetailsAuthoringService::CaptureStructDeltaFields(
        Struct, Value, NativeDefault.GetStructMemory(), RootOwnerPath, Owner, Fields, OutError)) {
      if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = RootOwnerPath;
      if (OutError.SemanticPointer.IsEmpty()) OutError.SemanticPointer = SemanticPointer;
      return false;
    }
  }
  auto Result = MakeShared<FJsonObject>();
  Result->SetObjectField(TEXT("fields"), Fields);
  OutValue = MakeShared<FJsonValueObject>(Result);
  return true;
}

bool DecodeStruct(UScriptStruct *Struct, const TSharedPtr<FJsonValue> &Value,
                  void *OutValue, UObject *Owner, UE::UAI::Contract::FDiagnostic& OutError) {
  FCodecOptions Options;
  Options.bAllowPartialStructFields = true;
  return UeAgentPropertyValueCodec::DecodeStructValue(Struct, Value, OutValue,
                                                      Owner, Options, OutError);
}

TSharedRef<FJsonObject> CloneObject(const TSharedPtr<FJsonObject> &Source) {
  return Source.IsValid()
             ? UeAgentDetailsAuthoring::CloneObjectTree(Source).ToSharedRef()
             : MakeShared<FJsonObject>();
}

template <typename T> TSharedRef<FJsonObject> MakeLiteral(const T Value) {
  TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
  TSharedPtr<FJsonValue> Literal;
  if constexpr (std::is_same_v<T, bool>) Literal = MakeShared<FJsonValueBoolean>(Value);
  else Literal = MakeShared<FJsonValueNumber>(Value);
  UeAgentAuthoringEncodedTypes::FScope::Record(Literal, UeAgentAuthoringTypeDescriptors::DescribeScalar<T>(), nullptr);
  Result->SetField(TEXT("literal"), Literal);
  return Result;
}

UE::UAI::Contract::FDiagnostic AssetTypeFailure(const TCHAR* Code, const TCHAR* Stage,
    const EAssetKind Kind, const UObject& Asset) {
  UClass* ExpectedClass = nullptr;
  switch (Kind) {
    case EAssetKind::ControlRig: ExpectedClass = UControlRigBlueprint::StaticClass(); break;
    case EAssetKind::IKRig: ExpectedClass = UIKRigDefinition::StaticClass(); break;
    case EAssetKind::IKRetargeter: ExpectedClass = UIKRetargeter::StaticClass(); break;
    default: break;
  }
  auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code,
      ExpectedClass ? TEXT("The native asset class does not match the requested Rigging document kind.")
          : TEXT("The requested Rigging document kind is unsupported."), Stage);
  Error.SourceKind = TEXT("native_result");
  Error.TargetPath = Asset.GetPathName();
  if (ExpectedClass) {
    Error.PropertyPath = TEXT("target_class");
    Error.ActualValue = MakeShared<FJsonValueString>(Asset.GetClass()->GetPathName());
    Error.ExpectedValue = MakeShared<FJsonValueString>(ExpectedClass->GetPathName());
  } else {
    Error.PropertyPath = TEXT("asset_kind");
    Error.ActualValue = MakeShared<FJsonValueNumber>(static_cast<int32>(Kind));
    TArray<TSharedPtr<FJsonValue>> Allowed;
    for (const auto Supported : {EAssetKind::ControlRig, EAssetKind::IKRig, EAssetKind::IKRetargeter})
      Allowed.Add(MakeShared<FJsonValueString>(AssetKindToString(Supported)));
    Error.ExpectedValue = MakeShared<FJsonValueArray>(MoveTemp(Allowed));
  }
  return Error;
}

UE::UAI::Contract::FDiagnostic InputFailure(const FString& Code, const FString& Reason,
    const FJsonObject& Input, const FString& Pointer, const TSharedPtr<FJsonValue>& Actual,
    const TSharedPtr<FJsonValue>& Expected, const FString& TargetPath) {
  auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Pointer, Expected, Actual,
      TEXT("authoring_input"), Reason);
  Error.SourceKind = TEXT("authored_value");
  Error.Source.InternalJsonPath = Pointer;
  Error.TargetPath = TargetPath;
  Error.ActualValuePresent = Actual.IsValid();
  Error.ExpectedValuePresent = Expected.IsValid();
  UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, Input);
  return Error;
}

void DescribeNativeProjectionFailure(UE::UAI::Contract::FDiagnostic& Error, const FString& TargetPath,
    const FString& SemanticPointer) {
  Error.SourceKind = TEXT("native_result");
  if (Error.TargetPath.IsEmpty()) Error.TargetPath = TargetPath;
  if (Error.SemanticPointer.IsEmpty()) Error.SemanticPointer = SemanticPointer;
  for (auto& Child : Error.Diagnostics) DescribeNativeProjectionFailure(Child, TargetPath, SemanticPointer);
  if (Error.PublicCause) {
    auto Cause = MakeShared<UE::UAI::Contract::FDiagnostic>(*Error.PublicCause);
    DescribeNativeProjectionFailure(*Cause, TargetPath, SemanticPointer);
    Error.PublicCause = MoveTemp(Cause);
  }
}

UE::UAI::Contract::FDiagnostic NativeDocumentFailure(const TCHAR* Code, const FString& Reason,
    const FString& Target, const FString& Pointer, const TSharedPtr<FJsonValue>& Actual,
    const TSharedPtr<FJsonValue>& Expected) {
  auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Pointer, Expected, Actual,
      TEXT("native_apply"), Reason);
  DescribeNativeProjectionFailure(Error, Target, Pointer);
  Error.Source = {};
  Error.ActualValuePresent = Actual.IsValid();
  Error.ExpectedValuePresent = Expected.IsValid();
  return Error;
}

void BindNativeCaptureTarget(UE::UAI::Contract::FDiagnostic& Error, const UObject& Asset, const FString& TargetPath) {
  if (Error.IsEmpty()) return;
  const FString NativePath = Asset.GetPathName();
  if (Error.InternalContext.IsEmpty()) Error.InternalContext = NativePath;
  if (Error.SourceKind.IsEmpty()) Error.SourceKind = TEXT("native_result");
  if (!TargetPath.IsEmpty()) {
    if (Error.TargetPath.IsEmpty() || Error.TargetPath == NativePath) Error.TargetPath = TargetPath;
    else if (Error.TargetPath.StartsWith(NativePath + TEXT(":")))
      Error.TargetPath = TargetPath + Error.TargetPath.Mid(NativePath.Len());
  }
  for (auto& Child : Error.Diagnostics) BindNativeCaptureTarget(Child, Asset, TargetPath);
  if (Error.PublicCause) {
    auto Cause = MakeShared<UE::UAI::Contract::FDiagnostic>(*Error.PublicCause);
    BindNativeCaptureTarget(*Cause, Asset, TargetPath);
    Error.PublicCause = MoveTemp(Cause);
  }
}

void LocateDocumentValueFailure(UE::UAI::Contract::FDiagnostic& Error, const FJsonObject& Document,
    const FString& Pointer, const FString& Target) {
  if (Error.SourceKind == TEXT("native_result")) {
    if (Error.SemanticPointer.IsEmpty()) Error.SemanticPointer = Pointer;
  } else if (Error.SourceKind != TEXT("command_input")) {
    UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(Error, Pointer);
  }
  UeAgentAuthoringDiagnostics::LocateFailure(Error, Document, TEXT("input_decode"), Target);
}

UE::UAI::Contract::FDiagnostic LiteralFailure(const TCHAR* Code, const TCHAR* Reason,
    const TSharedPtr<FJsonValue>& Actual, const TCHAR* Expected, const TCHAR* Pointer = TEXT("/")) {
  auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Pointer,
      MakeShared<FJsonValueString>(Expected), Actual, TEXT("input_decode"), Reason);
  Error.SourceKind = TEXT("authored_value");
  Error.Source.InternalJsonPath = Pointer;
  Error.ActualValuePresent = Actual.IsValid();
  return Error;
}

bool ReadLiteral(const TSharedPtr<FJsonValue> &Value, bool &OutValue,
                 UE::UAI::Contract::FDiagnostic& OutError) {
  const TSharedPtr<FJsonObject> Object =
      Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
  if (!Object.IsValid() || Object->Values.Num() != 1) {
    OutError = LiteralFailure(TEXT("expected_boolean_literal"),
        TEXT("A boolean control value must be an object containing only the literal field."),
        Value, TEXT("object with exactly one boolean literal field"));
    return false;
  }
  if (!Object->TryGetBoolField(TEXT("literal"), OutValue)) {
    OutError = LiteralFailure(TEXT("expected_boolean_literal"),
        TEXT("The literal field must contain a boolean."), Object->TryGetField(TEXT("literal")),
        TEXT("boolean"), TEXT("/literal"));
    return false;
  }
  return true;
}

bool ReadLiteral(const TSharedPtr<FJsonValue> &Value, double &OutValue,
                 UE::UAI::Contract::FDiagnostic& OutError) {
  const TSharedPtr<FJsonObject> Object =
      Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
  if (!Object.IsValid() || Object->Values.Num() != 1) {
    OutError = LiteralFailure(TEXT("expected_numeric_literal"),
        TEXT("A numeric control or curve value must be an object containing only the literal field."),
        Value, TEXT("object with exactly one numeric literal field"));
    return false;
  }
  if (!Object->TryGetNumberField(TEXT("literal"), OutValue)) {
    OutError = LiteralFailure(TEXT("expected_numeric_literal"),
        TEXT("The literal field must contain a number."), Object->TryGetField(TEXT("literal")),
        TEXT("finite number"), TEXT("/literal"));
    return false;
  }
  if (!FMath::IsFinite(OutValue)) {
    OutError = LiteralFailure(TEXT("expected_numeric_literal"),
        TEXT("The numeric literal must be finite."), MakeShared<FJsonValueString>(LexToString(OutValue)),
        TEXT("finite number"), TEXT("/literal"));
    return false;
  }
  return true;
}

bool ReadLiteral(const TSharedPtr<FJsonValue>& Value, float& OutValue, UE::UAI::Contract::FDiagnostic& OutError) {
  double Number = 0.0;
  if (!ReadLiteral(Value, Number, OutError)) return false;
  if (!UeAgentPropertyValueCodec::TryConvertFloat32(Number, OutValue, OutError)) {
    UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(OutError, TEXT("/literal"));
    return false;
  }
  return true;
}

bool ValidateFields(const TSharedPtr<const FJsonObject> &Object,
                    const TSet<FString> &Allowed, const FString &Prefix,
                    UE::UAI::Contract::FDiagnostic& OutError, const FString& TargetPath,
                    const FString& DocumentPointer = FString()) {
  if (!Object.IsValid())
    return true;
  for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair : Object->Values) {
    if (!Allowed.Contains(Pair.Key)) {
      TArray<FString> Names = Allowed.Array();
      Names.Sort();
      TArray<TSharedPtr<FJsonValue>> Expected;
      for (const auto& Name : Names) Expected.Add(MakeShared<FJsonValueString>(Name));
      OutError = InputFailure(Prefix + TEXT("_unknown_field"), TEXT("Unknown field: ") + Pair.Key,
          *Object, TEXT("/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Pair.Key),
          MakeShared<FJsonValueString>(Pair.Key), MakeShared<FJsonValueArray>(MoveTemp(Expected)), TargetPath);
      UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(OutError, DocumentPointer);
      return false;
    }
  }
  return true;
}

FString ElementKind(const ERigElementType Type) {
  switch (Type) {
  case ERigElementType::Bone:
    return TEXT("bone");
  case ERigElementType::Null:
    return TEXT("null");
  case ERigElementType::Control:
    return TEXT("control");
  case ERigElementType::Curve:
    return TEXT("curve");
  case ERigElementType::Connector:
    return TEXT("connector");
  case ERigElementType::Socket:
    return TEXT("socket");
  default:
    return TEXT("element");
  }
}

FString ElementId(const FRigElementKey &Key) {
  return ElementKind(Key.Type) + TEXT(":") + Key.Name.ToString();
}

bool ParseElementKind(const FString &Kind, ERigElementType &OutType) {
  if (Kind == TEXT("bone"))
    OutType = ERigElementType::Bone;
  else if (Kind == TEXT("null"))
    OutType = ERigElementType::Null;
  else if (Kind == TEXT("control"))
    OutType = ERigElementType::Control;
  else if (Kind == TEXT("curve"))
    OutType = ERigElementType::Curve;
  else if (Kind == TEXT("connector"))
    OutType = ERigElementType::Connector;
  else if (Kind == TEXT("socket"))
    OutType = ERigElementType::Socket;
  else
    return false;
  return true;
}

TSharedPtr<FJsonValue> EncodeControlValue(const FRigControlValue &Value,
                                          const FRigControlSettings &Settings) {
  switch (Settings.ControlType) {
  case ERigControlType::Bool:
    return MakeShared<FJsonValueObject>(MakeLiteral(Value.Get<bool>()));
  case ERigControlType::Float:
  case ERigControlType::ScaleFloat:
    return MakeShared<FJsonValueObject>(MakeLiteral(Value.Get<float>()));
  case ERigControlType::Integer:
    return MakeShared<FJsonValueObject>(
        MakeLiteral(Value.Get<int32>()));
  case ERigControlType::Vector2D: {
    const FVector3f Stored = Value.Get<FVector3f>();
    const FVector2f Canonical(Stored.X, Stored.Y);
    return EncodeStruct(TVariantStructure<FVector2f>::Get(), &Canonical);
  }
  case ERigControlType::Position:
  case ERigControlType::Scale: {
    const FVector3f Canonical(Value.Get<FVector3f>());
    return EncodeStruct(TVariantStructure<FVector3f>::Get(), &Canonical);
  }
  case ERigControlType::Rotator: {
    const FRotator3f Canonical = FRotator3f::MakeFromEuler(Value.Get<FVector3f>());
    return EncodeStruct(TVariantStructure<FRotator3f>::Get(), &Canonical);
  }
  case ERigControlType::Transform:
  case ERigControlType::TransformNoScale:
  case ERigControlType::EulerTransform: {
    const FTransform3f Canonical(Value.GetAsTransform(Settings.ControlType, Settings.PrimaryAxis));
    return EncodeStruct(TVariantStructure<FTransform3f>::Get(), &Canonical);
  }
  default:
    return MakeShared<FJsonValueNull>();
  }
}

bool DecodeControlValue(const TSharedPtr<FJsonValue> &Value,
                        const FRigControlSettings &Settings,
                        FRigControlValue &OutValue, UE::UAI::Contract::FDiagnostic& OutError) {
  switch (Settings.ControlType) {
  case ERigControlType::Bool: {
    bool Decoded = false;
    if (!ReadLiteral(Value, Decoded, OutError))
      return false;
    OutValue.Set<bool>(Decoded);
    return true;
  }
  case ERigControlType::Float:
  case ERigControlType::ScaleFloat: {
    float Decoded = 0.0f;
    if (!ReadLiteral(Value, Decoded, OutError))
      return false;
    OutValue.Set<float>(Decoded);
    return true;
  }
  case ERigControlType::Integer: {
    int32 Decoded = 0;
    if (!Value.IsValid() || Value->Type != EJson::Object || Value->AsObject()->Values.Num() != 1
        || !Value->AsObject()->HasField(TEXT("literal"))) {
      OutError = LiteralFailure(TEXT("expected_numeric_literal"),
          TEXT("An integer control value must be an object containing only the literal field."),
          Value, TEXT("object with exactly one integer literal field"));
      return false;
    }
    if (!UeAgentAuthoringJsonFields::ReadInt32Field(*Value->AsObject(), TEXT("literal"), Decoded, OutError)) return false;
    OutValue.Set<int32>(Decoded);
    return true;
  }
  case ERigControlType::Vector2D: {
    FVector2f Decoded = FVector2f::ZeroVector;
    if (!DecodeStruct(TVariantStructure<FVector2f>::Get(), Value, &Decoded,
                      nullptr, OutError))
      return false;
    OutValue.Set<FVector3f>(FVector3f(Decoded.X, Decoded.Y, 0.0f));
    return true;
  }
  case ERigControlType::Position:
  case ERigControlType::Scale: {
    FVector3f Decoded = FVector3f::ZeroVector;
    if (!DecodeStruct(TVariantStructure<FVector3f>::Get(), Value, &Decoded, nullptr,
                      OutError))
      return false;
    OutValue.Set<FVector3f>(Decoded);
    return true;
  }
  case ERigControlType::Rotator: {
    FRotator3f Decoded = FRotator3f::ZeroRotator;
    if (!DecodeStruct(TVariantStructure<FRotator3f>::Get(), Value, &Decoded, nullptr,
                      OutError))
      return false;
    OutValue.Set<FVector3f>(FVector3f(Decoded.Euler()));
    return true;
  }
  case ERigControlType::Transform:
  case ERigControlType::TransformNoScale:
  case ERigControlType::EulerTransform: {
    FTransform3f Decoded = FTransform3f::Identity;
    if (!DecodeStruct(TVariantStructure<FTransform3f>::Get(), Value, &Decoded,
                      nullptr, OutError))
      return false;
    OutValue.SetFromTransform(FTransform(Decoded), Settings.ControlType,
                              Settings.PrimaryAxis);
    return true;
  }
  default:
    OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("unsupported_control_value_type"),
        TEXT("The resolved Control Rig control type is unsupported for document value decoding."), TEXT("input_decode"));
    OutError.SourceKind = TEXT("native_result");
    OutError.PropertyPath = TEXT("control_type");
    OutError.ActualValue = MakeShared<FJsonValueNumber>(static_cast<int32>(Settings.ControlType));
    OutError.ExpectedValue = MakeShared<FJsonValueString>(
        TEXT("Bool, Float, ScaleFloat, Integer, Vector2D, Position, Scale, Rotator, Transform, TransformNoScale or EulerTransform"));
    return false;
  }
}

TSharedRef<FJsonObject> BuildRigElement(URigHierarchy &Hierarchy,
                                        const FRigElementKey &Key) {
  TSharedRef<FJsonObject> Element = MakeShared<FJsonObject>();
  Element->SetStringField(TEXT("kind"), ElementKind(Key.Type));
  UeAgentAuthoringEncodedTypes::FScope::SetNameField(Element, TEXT("name"), Key.Name);
  const FRigElementKey Parent = Hierarchy.GetFirstParent(Key);
  if (Parent.IsValid()) {
    TSharedRef<FJsonObject> ParentObject = MakeShared<FJsonObject>();
    ParentObject->SetStringField(TEXT("kind"), ElementKind(Parent.Type));
    UeAgentAuthoringEncodedTypes::FScope::SetNameField(ParentObject, TEXT("name"), Parent.Name);
    Element->SetObjectField(TEXT("parent"), ParentObject);
  } else {
    Element->SetField(TEXT("parent"), MakeShared<FJsonValueNull>());
  }
  return Element;
}

FString ControlRigTypeName(const UControlRigBlueprint& Blueprint) {
  return Blueprint.ControlRigType == EControlRigType::RigModule ? TEXT("rig_module")
      : Blueprint.ControlRigType == EControlRigType::ModularRig ? TEXT("modular_rig") : TEXT("control_rig");
}

using FControlRigNativeLinks = TMap<FString, TMap<FString, TPair<FString, FString>>>;
struct FControlRigGraphEditContext {
  UE::UAI::HjsonWorkspace::FGraphAuthoringConnectionContext Connections;
  UE::UAI::HjsonWorkspace::FGraphAuthoringAliasProjection NodeAliases;
  TSet<FString> UnchangedNodes;
  TArray<TPair<FString, FString>> RemovedLinks;
};
using FControlRigGraphEditContexts = TMap<FString, FControlRigGraphEditContext>;

bool BuildControlRigDocument(UControlRigBlueprint &Blueprint,
                             TSharedPtr<FJsonObject> &OutDocument,
                             UE::UAI::Contract::FDiagnostic& OutError, FControlRigNativeLinks* OutNativeLinks = nullptr,
                             FControlRigGraphEditContexts* OutGraphContexts = nullptr) {
  OutError.Reset();
  TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
  Document->SetStringField(TEXT("rig_type"), ControlRigTypeName(Blueprint));
  Document->SetStringField(TEXT("preview_mesh"),
                           Blueprint.GetPreviewMesh()
                               ? Blueprint.GetPreviewMesh()->GetPathName()
                               : FString());
  TArray<TSharedPtr<FJsonValue>> ShapeLibraries;
  for (const TSoftObjectPtr<UControlRigShapeLibrary> &Library :
       Blueprint.ShapeLibraries) {
    ShapeLibraries.Add(
        MakeShared<FJsonValueString>(Library.ToSoftObjectPath().ToString()));
  }
  Document->SetArrayField(TEXT("shape_libraries"), ShapeLibraries);

  TSharedRef<FJsonObject> HierarchyObject = MakeShared<FJsonObject>();
  HierarchyObject->SetStringField(TEXT("transform_space"),
                                  TEXT("parent_local_initial"));
  TSharedRef<FJsonObject> Elements = MakeShared<FJsonObject>();
  URigHierarchy *Hierarchy = Blueprint.GetHierarchy();
  if (Hierarchy) {
    for (const FRigElementKey &Key : Hierarchy->GetAllKeys(true)) {
      if (Key.Type != ERigElementType::Bone &&
          Key.Type != ERigElementType::Null &&
          Key.Type != ERigElementType::Control &&
          Key.Type != ERigElementType::Curve &&
          Key.Type != ERigElementType::Connector &&
          Key.Type != ERigElementType::Socket)
        continue;
      TSharedRef<FJsonObject> Element = BuildRigElement(*Hierarchy, Key);
      if (Key.Type == ERigElementType::Bone ||
          Key.Type == ERigElementType::Null || Key.Type == ERigElementType::Socket) {
        const FTransform InitialLocal = Hierarchy->GetLocalTransform(Key, true);
        Element->SetField(TEXT("initial_local_transform"),
                          EncodeStruct(TBaseStructure<FTransform>::Get(),
                                       &InitialLocal, &Blueprint));
      }
      if (const FRigBoneElement *Bone = Hierarchy->Find<FRigBoneElement>(Key)) {
        Element->SetStringField(
            TEXT("bone_type"), StaticEnum<ERigBoneType>()->GetNameStringByValue(
                                   static_cast<int64>(Bone->BoneType)));
      }
      if (const FRigControlElement *Control =
              Hierarchy->Find<FRigControlElement>(Key)) {
        TSharedPtr<FJsonValue> Settings;
        if (!TryCaptureSettingsDelta(FRigControlSettings::StaticStruct(), &Control->Settings, &Blueprint,
            TEXT("/hierarchy/elements/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(ElementId(Key)) + TEXT("/settings"),
            Settings, OutError)) return false;
        Element->SetField(TEXT("settings"), Settings);
        const FRigControlValue InitialValue =
            Hierarchy->GetControlValue(Key, ERigControlValueType::Initial);
        Element->SetField(TEXT("initial_value"),
                          EncodeControlValue(InitialValue, Control->Settings));
        const FTransform Offset = Hierarchy->GetControlOffsetTransform(
            const_cast<FRigControlElement *>(Control),
            ERigTransformType::InitialLocal);
        const FTransform Shape = Hierarchy->GetControlShapeTransform(
            const_cast<FRigControlElement *>(Control),
            ERigTransformType::InitialLocal);
        Element->SetField(TEXT("initial_offset_transform"),
                          EncodeStruct(TBaseStructure<FTransform>::Get(),
                                       &Offset, &Blueprint));
        Element->SetField(TEXT("initial_shape_transform"),
                          EncodeStruct(TBaseStructure<FTransform>::Get(),
                                       &Shape, &Blueprint));
      }
      if (Key.Type == ERigElementType::Curve) {
        Element->SetObjectField(TEXT("value"),
                                MakeLiteral(Hierarchy->GetCurveValue(Key)));
      }
      if (const auto* Connector = Hierarchy->Find<FRigConnectorElement>(Key)) {
        TSharedPtr<FJsonValue> Settings;
        if (!TryCaptureSettingsDelta(FRigConnectorSettings::StaticStruct(), &Connector->Settings, &Blueprint,
            TEXT("/hierarchy/elements/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(ElementId(Key)) + TEXT("/settings"),
            Settings, OutError)) return false;
        Element->SetField(TEXT("settings"), Settings);
      }
      if (const auto* Socket = Hierarchy->Find<FRigSocketElement>(Key)) {
        const FLinearColor Color = Socket->GetColor(Hierarchy);
        Element->SetField(TEXT("color"), EncodeStruct(TBaseStructure<FLinearColor>::Get(), &Color, &Blueprint));
        Element->SetStringField(TEXT("description"), Socket->GetDescription(Hierarchy));
      }
      Elements->SetObjectField(ElementId(Key), Element);
    }
  }
  UeAgentAuthoringEncodedTypes::FScope::RecordCompositeKeyedObject(Elements, TEXT(":"), {TEXT("string"), TEXT("name")});
  HierarchyObject->SetObjectField(TEXT("elements"), Elements);
  Document->SetObjectField(TEXT("hierarchy"), HierarchyObject);

  TSharedRef<FJsonObject> GraphsObject = MakeShared<FJsonObject>();
  GraphsObject->SetStringField(TEXT("position_space"),
                               TEXT("rigvm_graph_canvas_2d"));
  TSharedRef<FJsonObject> Graphs = MakeShared<FJsonObject>();
  for (URigVMGraph *Graph : Blueprint.GetAllModels()) {
    if (!Graph)
      continue;
    TSharedRef<FJsonObject> GraphObject = MakeShared<FJsonObject>();
    TSharedRef<FJsonObject> Nodes = MakeShared<FJsonObject>();
    using namespace UE::UAI::HjsonWorkspace;
    TArray<FGraphAuthoringNodeAliasInput> NodeAliasInputs;
    for (URigVMNode *Node : Graph->GetNodes()) {
      if (Node) NodeAliasInputs.Add({Node->GetName(), Node->GetNodeTitle()});
    }
    FGraphAuthoringAliasProjection NodeAliases;
    FWorkspaceError AliasError;
    if (!TryBuildGraphAuthoringAliasProjection(NodeAliasInputs, NodeAliases, AliasError)) {
      OutError = AliasError.GetDiagnostic();
      DescribeNativeProjectionFailure(OutError, UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint),
          TEXT("/graphs/graphs/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Graph->GetNodePath()));
      return false;
    }
    if (OutGraphContexts) OutGraphContexts->FindOrAdd(Graph->GetNodePath()).NodeAliases = NodeAliases;
    TMap<FString, FString> PinAliasByPath;
    for (URigVMNode *Node : Graph->GetNodes()) {
      if (!Node)
        continue;
      TSharedRef<FJsonObject> NodeObject = MakeShared<FJsonObject>();
      const FString NodeAlias = NodeAliases.AliasBySourceId.FindRef(Node->GetName());
      NodeObject->SetStringField(TEXT("name"), NodeAlias.IsEmpty() ? Node->GetName() : NodeAlias);
      const FVector2D Position = Node->GetPosition();
      NodeObject->SetField(TEXT("position"),
                           EncodeStruct(TBaseStructure<FVector2D>::Get(),
                                        &Position, &Blueprint));
      UeAgentRigVMNodeAuthoring::CaptureDefinition(Blueprint, *Node, *NodeObject);
      TArray<FGraphAuthoringPinAliasInput> PinAliasInputs;
      TMap<FString, int32> PinOccurrences;
      for (URigVMPin *Pin : Node->GetAllPinsRecursively()) {
        if (!Pin || Pin->GetDirection() == ERigVMPinDirection::Hidden) continue;
        const EGraphAuthoringEndpointDirection Direction =
          Pin->GetDirection() == ERigVMPinDirection::Output
            ? EGraphAuthoringEndpointDirection::Output
            : EGraphAuthoringEndpointDirection::Input;
        const FString OccurrenceKey = Pin->GetName() + TEXT("\n")
          + (Direction == EGraphAuthoringEndpointDirection::Output ? TEXT("output") : TEXT("input"));
        int32& Occurrence = PinOccurrences.FindOrAdd(OccurrenceKey);
        PinAliasInputs.Add({Pin->GetPinPath(false), Pin->GetName(), Occurrence, Direction});
        ++Occurrence;
      }
      FGraphAuthoringPinAliasProjection PinAliases;
      if (!TryBuildGraphAuthoringPinAliasProjection(PinAliasInputs, PinAliases, AliasError)) {
        OutError = AliasError.GetDiagnostic();
        DescribeNativeProjectionFailure(OutError, UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint),
            TEXT("/graphs/graphs/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Graph->GetNodePath()));
        return false;
      }
      TSharedRef<FJsonObject> PinInterface = MakeShared<FJsonObject>();
      TArray<TSharedPtr<FJsonValue>> PinValues;
      TSharedRef<FJsonObject> PinDefaults = MakeShared<FJsonObject>();
      for (URigVMPin *Pin : Node->GetAllPinsRecursively()) {
        if (!Pin || Pin->GetDirection() == ERigVMPinDirection::Hidden) continue;
        const FString Alias = PinAliases.AliasBySourceId.FindRef(Pin->GetPinPath(false));
        if (Alias.IsEmpty()) continue;
        const FString PinPath = Pin->GetPinPath(false);
        const FString NativeEndpoint = PinPath.StartsWith(Node->GetName() + TEXT("."))
          ? PinPath : Node->GetName() + TEXT(".") + PinPath;
        FString VisibleEndpoint;
        if (!TryFormatGraphAuthoringEndpoint(
                NodeAlias, Alias, 0,
                Pin->GetDirection() == ERigVMPinDirection::Output
                  ? EGraphAuthoringEndpointDirection::Output
                  : EGraphAuthoringEndpointDirection::Input,
                VisibleEndpoint, AliasError)) {
          OutError = AliasError.GetDiagnostic();
          DescribeNativeProjectionFailure(OutError, UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint),
              TEXT("/graphs/graphs/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Graph->GetNodePath()));
          return false;
        }
        PinAliasByPath.Add(NativeEndpoint, VisibleEndpoint);
        PinAliasByPath.Add(PinPath, VisibleEndpoint);
        TSharedRef<FJsonObject> PinValue = MakeShared<FJsonObject>();
        PinValue->SetStringField(TEXT("alias"), Alias);
        PinValue->SetStringField(TEXT("direction"),
          Pin->GetDirection() == ERigVMPinDirection::Output ? TEXT("output") : TEXT("input"));
        PinValue->SetStringField(TEXT("cpp_type"), Pin->GetCPPType());
        PinValues.Add(MakeShared<FJsonValueObject>(PinValue));
        if (UeAgentRigVMNodeAuthoring::IsAuthorableDefaultPin(*Pin))
          PinDefaults->SetStringField(Alias, Pin->GetDefaultValue());
      }
      PinInterface->SetArrayField(TEXT("pins"), PinValues);
      NodeObject->SetObjectField(TEXT("pin_interface"), PinInterface);
      NodeObject->SetObjectField(TEXT("pin_defaults"), PinDefaults);
      Nodes->SetObjectField(NodeAlias.IsEmpty() ? Node->GetName() : NodeAlias, NodeObject);
    }
    GraphObject->SetObjectField(TEXT("nodes"), Nodes);
    TArray<TSharedPtr<FJsonValue>> Links;
    for (URigVMLink *Link : Graph->GetLinks()) {
      if (!Link)
        continue;
      TArray<TSharedPtr<FJsonValue>> Pair;
      const FString *Source = PinAliasByPath.Find(Link->GetSourcePinPath());
      const FString *Target = PinAliasByPath.Find(Link->GetTargetPinPath());
      if (!Source || !Target) {
        auto Actual = MakeShared<FJsonObject>();
        Actual->SetStringField(TEXT("source_pin"), Link->GetSourcePinPath());
        Actual->SetStringField(TEXT("target_pin"), Link->GetTargetPinPath());
        Actual->SetBoolField(TEXT("source_alias_available"), Source != nullptr);
        Actual->SetBoolField(TEXT("target_alias_available"), Target != nullptr);
        OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("control_rig_graph_link_alias_missing"),
            TEXT("link_endpoints"), MakeShared<FJsonValueString>(TEXT("authoring aliases for both native link endpoints")),
            MakeShared<FJsonValueObject>(Actual), TEXT("state_capture"),
            TEXT("A native graph connection cannot be represented because an endpoint has no authoring alias."));
        DescribeNativeProjectionFailure(OutError, UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint),
            TEXT("/graphs/graphs/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Graph->GetNodePath()) + TEXT("/links"));
        return false;
      }
      Pair.Add(MakeShared<FJsonValueString>(*Source));
      Pair.Add(MakeShared<FJsonValueString>(*Target));
      Links.Add(MakeShared<FJsonValueArray>(Pair));
      if (OutNativeLinks) OutNativeLinks->FindOrAdd(Graph->GetNodePath()).Add(*Source + TEXT("\n") + *Target,
          {Link->GetSourcePinPath(), Link->GetTargetPinPath()});
    }
    // Native link enumeration is not graph execution order. Keep the authored
    // endpoint pair ordered, but declare the collection at its producer so all
    // typed readback consumers compare relations without reordering the payload.
    const TSharedPtr<FJsonValue> LinkValue = MakeShared<FJsonValueArray>(MoveTemp(Links));
    UeAgentAuthoringTypeDescriptors::FDescriptor LinkType;
    LinkType.Kind = TEXT("set");
    LinkType.ElementType = MakeShared<UeAgentAuthoringTypeDescriptors::FDescriptor>();
    LinkType.ElementType->Kind = TEXT("fixed_array");
    LinkType.ElementType->FixedLength = 2;
    LinkType.ElementType->ElementType = MakeShared<UeAgentAuthoringTypeDescriptors::FDescriptor>();
    LinkType.ElementType->ElementType->Kind = TEXT("string");
    UeAgentAuthoringEncodedTypes::FScope::Record(LinkValue, LinkType, nullptr);
    GraphObject->SetField(TEXT("links"), LinkValue);
    Graphs->SetObjectField(Graph->GetNodePath(), GraphObject);
  }
  GraphsObject->SetObjectField(TEXT("graphs"), Graphs);
  Document->SetObjectField(TEXT("graphs"), GraphsObject);
  Document->SetObjectField(
      TEXT("variables"), UeAgentControlRigOps::BuildVariablesJson(&Blueprint));
  OutDocument = Document;
  return true;
}

bool ApplyControlRigHierarchy(
    UControlRigBlueprint &Blueprint,
    const TSharedPtr<const FJsonObject> &HierarchyObject,
    FMutationStats &OutStats, UE::UAI::Contract::FDiagnostic& OutError) {
  if (!HierarchyObject.IsValid())
    return true;
  const auto FailInput = [&](const TCHAR* Code, const TCHAR* Reason, const FString& Pointer,
      const TSharedPtr<FJsonValue>& Actual, const FString& Expected) {
    OutError = InputFailure(Code, Reason, *HierarchyObject, Pointer, Actual,
        MakeShared<FJsonValueString>(Expected), UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint));
    UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(OutError, TEXT("/hierarchy"));
    return false;
  };
  if (!ValidateFields(HierarchyObject,
                      {TEXT("transform_space"), TEXT("elements")},
                      TEXT("control_rig_hierarchy"), OutError, UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint), TEXT("/hierarchy")))
    return false;
  FString Space;
  if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(*HierarchyObject,
      {{TEXT("transform_space"), EJson::String}, {TEXT("elements"), EJson::Object}}, OutError, TEXT("/hierarchy"))) return false;
  if (HierarchyObject->TryGetStringField(TEXT("transform_space"), Space) &&
      Space != TEXT("parent_local_initial")) {
    return FailInput(TEXT("control_rig_hierarchy_transform_space_invalid"),
        TEXT("Hierarchy transforms must use the parent-local initial coordinate space."),
        TEXT("/transform_space"), MakeShared<FJsonValueString>(Space), TEXT("parent_local_initial"));
  }
  const TSharedPtr<FJsonObject> *Elements = nullptr;
  if (!HierarchyObject->TryGetObjectField(TEXT("elements"), Elements))
    return true;
  if (!Elements || !Elements->IsValid()) {
    return FailInput(TEXT("control_rig_hierarchy_elements_invalid"),
        TEXT("Hierarchy elements must be an object keyed by element identity."),
        TEXT("/elements"), HierarchyObject->TryGetField(TEXT("elements")), TEXT("object"));
  }
  URigHierarchy *Hierarchy = Blueprint.GetHierarchy();
  URigHierarchyController *Controller =
      Hierarchy ? Hierarchy->GetController(true) : nullptr;
  if (!Hierarchy || !Controller) {
    OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(
        TEXT("control_rig_hierarchy_controller_unavailable"), Hierarchy ? TEXT("controller_available") : TEXT("hierarchy_available"),
        MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), TEXT("hierarchy_apply"),
        Hierarchy ? TEXT("The native Control Rig hierarchy has no editing controller.")
            : TEXT("The Control Rig Blueprint has no native hierarchy."));
    OutError.SourceKind = TEXT("native_result");
    OutError.TargetPath = UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint);
    return false;
  }

  TSet<FRigElementKey> Desired;
  TMap<FRigElementKey, ERigBoneType> BoneTypes;
  for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair :
       (*Elements)->Values) {
    const TSharedPtr<FJsonObject> Element =
        Pair.Value.IsValid() && Pair.Value->Type == EJson::Object ? Pair.Value->AsObject() : nullptr;
    const FString InputPointer = TEXT("/elements/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Pair.Key);
    FString Kind;
    FString Name;
    ERigElementType Type = ERigElementType::None;
    if (!Element.IsValid())
      return FailInput(TEXT("control_rig_element_identity_invalid"), TEXT("Each hierarchy element must be an object."),
          InputPointer, Pair.Value, TEXT("object with kind and non-empty name"));
    if (!Element->TryGetStringField(TEXT("kind"), Kind))
      return FailInput(TEXT("control_rig_element_identity_invalid"), TEXT("The element kind must be a string."),
          InputPointer + TEXT("/kind"), Element->TryGetField(TEXT("kind")), TEXT("bone, null, control, curve, connector or socket"));
    if (!Element->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
      return FailInput(TEXT("control_rig_element_identity_invalid"), TEXT("The element name must be a non-empty string."),
          InputPointer + TEXT("/name"), Element->TryGetField(TEXT("name")), TEXT("non-empty string"));
    if (!ParseElementKind(Kind, Type))
      return FailInput(TEXT("control_rig_element_identity_invalid"), TEXT("The element kind is unsupported."),
          InputPointer + TEXT("/kind"), MakeShared<FJsonValueString>(Kind), TEXT("bone, null, control, curve, connector or socket"));
    const FRigElementKey Key(FName(*Name), Type);
    if (Pair.Key != ElementId(Key) || Desired.Contains(Key)) {
      return FailInput(TEXT("control_rig_element_key_invalid_or_duplicate"),
          Pair.Key != ElementId(Key) ? TEXT("The element key does not match its kind and name.")
              : TEXT("Another element already declares the same native identity."),
          InputPointer, MakeShared<FJsonValueString>(Pair.Key),
          Pair.Key != ElementId(Key) ? ElementId(Key) : TEXT("unique element identity"));
    }
    const FString ElementPointer = TEXT("/hierarchy/elements/") + Pair.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
    if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(*Element,
        {{TEXT("bone_type"), EJson::String}}, OutError, ElementPointer)) return false;
    if (Element->HasField(TEXT("bone_type"))) {
      int64 Value = 0;
      const FString BoneTypeText = Element->GetStringField(TEXT("bone_type"));
      if (Type != ERigElementType::Bone || !UeAgentAuthoringTypeDescriptors::TryResolveEnumValueName(
          StaticEnum<ERigBoneType>(), BoneTypeText, Value)) {
        OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("control_rig_bone_type_invalid"), TEXT("/bone_type"),
            MakeShared<FJsonValueString>(Type == ERigElementType::Bone ? StaticEnum<ERigBoneType>()->GetPathName() : TEXT("absent on non-bone elements")),
            MakeShared<FJsonValueString>(BoneTypeText), TEXT("authoring"), Type == ERigElementType::Bone
                ? TEXT("The selected bone type is not a member of the native enum.") : TEXT("Only bone elements may declare bone_type."));
        OutError.TargetPath = UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint);
        UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Element, TEXT("bone_type"));
        if (OutError.Source.SourceFile.IsEmpty()) UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, ElementPointer);
        return false;
      }
      const ERigBoneType RequestedType = static_cast<ERigBoneType>(Value);
      if (const FRigBoneElement* ExistingBone = Hierarchy->Find<FRigBoneElement>(Key);
          ExistingBone && ExistingBone->BoneType != RequestedType) {
        UeAgentAuthoringDiagnostics::FDiagnostic Diagnostic;
        Diagnostic.Code = TEXT("control_rig_existing_bone_type_read_only");
        Diagnostic.Stage = TEXT("document_prepare");
        Diagnostic.TargetPath = UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint);
        Diagnostic.PropertyPath = TEXT("bone_type");
        Diagnostic.Source.JsonPath = ElementPointer + TEXT("/bone_type");
        Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(StaticEnum<ERigBoneType>()->GetNameStringByValue(static_cast<int64>(ExistingBone->BoneType)));
        Diagnostic.ActualValue = MakeShared<FJsonValueString>(BoneTypeText);
        Diagnostic.PublicMessage = TEXT("An existing bone's native origin cannot be changed by property editing. bone_type is used when creating a bone.");
        OutError = Diagnostic;
        return false;
      }
      BoneTypes.Add(Key, RequestedType);
    }
    Desired.Add(Key);
  }

  TArray<FRigElementKey> ExistingKeys = Hierarchy->GetAllKeys(false);
  TMap<FRigElementKey, FRigElementKey> CapturedParents;
  TMap<FRigElementKey, FTransform> CapturedLocalTransforms, CapturedOffsets, CapturedShapes;
  TMap<FRigElementKey, FRigControlSettings> CapturedControlSettings;
  TMap<FRigElementKey, TSharedPtr<FJsonValue>> CapturedControlValues;
  for (const auto& Key : ExistingKeys) {
    CapturedParents.Add(Key, Hierarchy->GetFirstParent(Key));
    if (Key.Type == ERigElementType::Bone || Key.Type == ERigElementType::Null || Key.Type == ERigElementType::Socket)
      CapturedLocalTransforms.Add(Key, Hierarchy->GetLocalTransform(Key, true));
    if (auto* Control = Hierarchy->Find<FRigControlElement>(Key)) {
      CapturedOffsets.Add(Key, Hierarchy->GetControlOffsetTransform(Control, ERigTransformType::InitialLocal));
      CapturedShapes.Add(Key, Hierarchy->GetControlShapeTransform(Control, ERigTransformType::InitialLocal));
      CapturedControlSettings.Add(Key, Control->Settings);
      CapturedControlValues.Add(Key, EncodeControlValue(
          Hierarchy->GetControlValue(Key, ERigControlValueType::Initial), Control->Settings));
    }
  }
  Algo::Reverse(ExistingKeys);
  for (const FRigElementKey &Key : ExistingKeys) {
    if (Desired.Contains(Key))
      continue;
    const bool bCanRemove = Key.Type == ERigElementType::Bone || Key.Type == ERigElementType::Control ||
                      Key.Type == ERigElementType::Null ||
                      Key.Type == ERigElementType::Curve || Key.Type == ERigElementType::Connector ||
                      Key.Type == ERigElementType::Socket;
    if (bCanRemove && !Controller->RemoveElement(Key, false, false)) {
      OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("control_rig_element_remove_failed"),
          TEXT("remove_succeeded"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false),
          TEXT("hierarchy_apply"), TEXT("The native hierarchy controller refused to remove the element."));
      OutError.SourceKind = TEXT("native_result");
      OutError.TargetPath = UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint);
      OutError.SemanticPointer = TEXT("/hierarchy/elements/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(ElementId(Key));
      return false;
    }
  }

  TSet<FString> Applied;
  while (Applied.Num() < (*Elements)->Values.Num()) {
    const int32 Before = Applied.Num();
    for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair :
         (*Elements)->Values) {
      if (Applied.Contains(Pair.Key))
        continue;
      const TSharedPtr<FJsonObject> Element = Pair.Value->AsObject();
      const auto FailElementField = [&](const TCHAR* Field) {
        const FString Pointer = TEXT("/hierarchy/elements/")
            + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Pair.Key) + TEXT("/") + Field;
        UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(OutError, Pointer);
        if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint);
        if (OutError.SourceKind == TEXT("native_result") && OutError.SemanticPointer.IsEmpty())
          OutError.SemanticPointer = Pointer;
        return false;
      };
      FString Kind;
      FString Name;
      Element->TryGetStringField(TEXT("kind"), Kind);
      Element->TryGetStringField(TEXT("name"), Name);
      ERigElementType Type = ERigElementType::None;
      ParseElementKind(Kind, Type);
      const FRigElementKey Key(FName(*Name), Type);
      const bool bExisting = Hierarchy->GetIndex(Key) != INDEX_NONE;
      FRigElementKey Parent;
      bool bParentEdited = Element->HasField(TEXT("parent"));
      const TSharedPtr<FJsonValue> ParentValue =
          Element->TryGetField(TEXT("parent"));
      if (ParentValue.IsValid() && ParentValue->Type != EJson::Null) {
        const TSharedPtr<FJsonObject> ParentObject = ParentValue->Type == EJson::Object ? ParentValue->AsObject() : nullptr;
        const FString ParentPointer = TEXT("/elements/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Pair.Key) + TEXT("/parent");
        FString ParentKind;
        FString ParentName;
        ERigElementType ParentType = ERigElementType::None;
        if (!ParentObject.IsValid())
          return FailInput(TEXT("control_rig_parent_identity_invalid"), TEXT("A parent identity must be an object or null."),
              ParentPointer, ParentValue, TEXT("object with kind and name, or null"));
        if (!ParentObject->TryGetStringField(TEXT("kind"), ParentKind))
          return FailInput(TEXT("control_rig_parent_identity_invalid"), TEXT("The parent kind must be a string."),
              ParentPointer + TEXT("/kind"), ParentObject->TryGetField(TEXT("kind")), TEXT("bone, null, control, curve, connector or socket"));
        if (!ParentObject->TryGetStringField(TEXT("name"), ParentName))
          return FailInput(TEXT("control_rig_parent_identity_invalid"), TEXT("The parent name must be a string."),
              ParentPointer + TEXT("/name"), ParentObject->TryGetField(TEXT("name")), TEXT("string"));
        if (!ParseElementKind(ParentKind, ParentType))
          return FailInput(TEXT("control_rig_parent_identity_invalid"), TEXT("The parent kind is unsupported."),
              ParentPointer + TEXT("/kind"), MakeShared<FJsonValueString>(ParentKind), TEXT("bone, null, control, curve, connector or socket"));
        Parent = FRigElementKey(FName(*ParentName), ParentType);
      }
      if (const auto* CapturedParent = CapturedParents.Find(Key); CapturedParent &&
          (!bParentEdited || *CapturedParent == Parent)) {
        // RemoveElement detaches children while maintaining their global pose.
        // An unchanged exported parent must not undo that native result.
        bParentEdited = false;
        Parent = Hierarchy->GetFirstParent(Key);
      }
      if (Parent.IsValid() && Hierarchy->GetIndex(Parent) == INDEX_NONE) continue;

      FTransform InitialLocal = FTransform::Identity;
      if (Type == ERigElementType::Bone || Type == ERigElementType::Null || Type == ERigElementType::Socket) {
        const TSharedPtr<FJsonValue> TransformValue =
            Element->TryGetField(TEXT("initial_local_transform"));
        if (TransformValue.IsValid() &&
            !DecodeStruct(TBaseStructure<FTransform>::Get(), TransformValue,
                          &InitialLocal, &Blueprint, OutError)) {
          return FailElementField(TEXT("initial_local_transform"));
        }
      }

      if (!bExisting) {
        if (Type == ERigElementType::Bone) {
          const ERigBoneType BoneType = BoneTypes.Contains(Key) ? BoneTypes[Key] : ERigBoneType::User;
          Controller->AddBone(Key.Name, Parent, InitialLocal, false, BoneType,
                              false, false);
        } else if (Type == ERigElementType::Null) {
          Controller->AddNull(Key.Name, Parent, InitialLocal, false, false,
                              false);
        } else if (Type == ERigElementType::Curve) {
          float CurveValue = 0.0f;
          const TSharedPtr<FJsonValue> Value =
              Element->TryGetField(TEXT("value"));
          if (Value.IsValid() && !ReadLiteral(Value, CurveValue, OutError))
            return FailElementField(TEXT("value"));
          Controller->AddCurve(Key.Name, CurveValue, false,
                               false);
          if (Value.IsValid()) Hierarchy->SetCurveValue(Key, CurveValue, false);
        } else if (Type == ERigElementType::Connector) {
          FRigConnectorSettings Settings;
          if (const auto Value = Element->TryGetField(TEXT("settings")); Value.IsValid() &&
              !DecodeStruct(FRigConnectorSettings::StaticStruct(), Value, &Settings, &Blueprint, OutError))
            return FailElementField(TEXT("settings"));
          Controller->AddConnector(Key.Name, Settings, false, false);
        } else if (Type == ERigElementType::Socket) {
          Controller->AddSocket(Key.Name, Parent, InitialLocal, false,
              FRigSocketElement::SocketDefaultColor, FString(), false, false);
        } else if (Type == ERigElementType::Control) {
          FRigControlSettings Settings;
          const TSharedPtr<FJsonValue> SettingsValue =
              Element->TryGetField(TEXT("settings"));
          if (SettingsValue.IsValid() &&
              !DecodeStruct(FRigControlSettings::StaticStruct(), SettingsValue,
                            &Settings, &Blueprint, OutError)) {
            return FailElementField(TEXT("settings"));
          }
          if (Settings.LimitEnabled.IsEmpty()) {
            Settings.SetupLimitArrayForType();
          }
          FRigControlValue ControlValue = Settings.GetIdentityValue();
          const TSharedPtr<FJsonValue> InitialValue =
              Element->TryGetField(TEXT("initial_value"));
          if (InitialValue.IsValid() &&
              !DecodeControlValue(InitialValue, Settings, ControlValue,
                                  OutError)) {
            return FailElementField(TEXT("initial_value"));
          }
          FTransform Offset = FTransform::Identity;
          FTransform Shape = FTransform::Identity;
          if (const TSharedPtr<FJsonValue> Value =
                  Element->TryGetField(TEXT("initial_offset_transform"));
              Value.IsValid() &&
              !DecodeStruct(TBaseStructure<FTransform>::Get(), Value, &Offset,
                            &Blueprint, OutError))
            return FailElementField(TEXT("initial_offset_transform"));
          if (const TSharedPtr<FJsonValue> Value =
                  Element->TryGetField(TEXT("initial_shape_transform"));
              Value.IsValid() &&
              !DecodeStruct(TBaseStructure<FTransform>::Get(), Value, &Shape,
                            &Blueprint, OutError))
            return FailElementField(TEXT("initial_shape_transform"));
          Controller->AddControl(Key.Name, Parent, Settings, ControlValue,
                                 Offset, Shape, false, false);
        }
      } else {
        if (bParentEdited && Hierarchy->GetFirstParent(Key) != Parent) {
          if (Parent.IsValid())
            Controller->SetParent(Key, Parent, false, false, false);
          else
            Controller->RemoveAllParents(Key, false, false, false);
        }
        if (Type == ERigElementType::Bone || Type == ERigElementType::Null || Type == ERigElementType::Socket) {
          const auto* CapturedLocal = CapturedLocalTransforms.Find(Key);
          if (Element->HasField(TEXT("initial_local_transform")) &&
              (!CapturedLocal || !CapturedLocal->Equals(InitialLocal))) {
            Hierarchy->SetLocalTransform(Key, InitialLocal, true, true, false, false);
            Hierarchy->SetLocalTransform(Key, InitialLocal, false, true, false, false);
          }
        } else if (Type == ERigElementType::Curve) {
          float CurveValue = 0.0f;
          if (const TSharedPtr<FJsonValue> Value =
                  Element->TryGetField(TEXT("value"));
              Value.IsValid()) {
            if (!ReadLiteral(Value, CurveValue, OutError)) return FailElementField(TEXT("value"));
            Hierarchy->SetCurveValue(Key, CurveValue, false);
          }
        } else if (Type == ERigElementType::Connector) {
          FRigConnectorSettings Settings = Hierarchy->FindChecked<FRigConnectorElement>(Key)->Settings;
          if (const auto Value = Element->TryGetField(TEXT("settings")); Value.IsValid()) {
            if (!DecodeStruct(FRigConnectorSettings::StaticStruct(), Value, &Settings, &Blueprint, OutError)) return FailElementField(TEXT("settings"));
            if (Settings != Hierarchy->FindChecked<FRigConnectorElement>(Key)->Settings)
              Hierarchy->SetConnectorSettings(Key, Settings, false);
          }
        } else if (Type == ERigElementType::Control) {
          FRigControlSettings Settings = Hierarchy->FindChecked<FRigControlElement>(Key)->Settings;
          const FRigControlSettings NativeSettings = Settings;
          if (const auto Value = Element->TryGetField(TEXT("settings")); Value.IsValid()) {
            const auto* BeforeSettings = CapturedControlSettings.Find(Key);
            FRigControlSettings Edited = BeforeSettings ? *BeforeSettings : Settings;
            if (!DecodeStruct(FRigControlSettings::StaticStruct(), Value, &Edited, &Blueprint, OutError)) return FailElementField(TEXT("settings"));
            if (BeforeSettings)
              UeAgentAuthoringObjectApply::ApplyStructChanges(FRigControlSettings::StaticStruct(),
                  BeforeSettings, &Edited, &Settings, &Blueprint);
            else Settings = MoveTemp(Edited);
            // The native setter reapplies control values and limits. Replaying
            // identical settings can erase a pose preserved by RemoveParent.
            if (!FRigControlSettings::StaticStruct()->CompareScriptStruct(&NativeSettings, &Settings, 0))
              Controller->SetControlSettings(Key, Settings, false);
          }
          FRigControlValue ControlValue;
          if (const auto Value = Element->TryGetField(TEXT("initial_value")); Value.IsValid()) {
            if (!DecodeControlValue(Value, Settings, ControlValue, OutError)) return FailElementField(TEXT("initial_value"));
            const auto BeforeValue = CapturedControlValues.FindRef(Key);
            const auto* BeforeSettings = CapturedControlSettings.Find(Key);
            if (!BeforeValue || !BeforeSettings || BeforeSettings->ControlType != Settings.ControlType
                || !FJsonValue::CompareEqual(*BeforeValue, *Value)) {
              Hierarchy->SetControlValue(Key, ControlValue, ERigControlValueType::Initial, false, false);
              Hierarchy->SetControlValue(Key, ControlValue, ERigControlValueType::Current, false, false);
            }
          }
          FTransform Offset = FTransform::Identity;
          FTransform Shape = FTransform::Identity;
          if (const auto Value = Element->TryGetField(TEXT("initial_offset_transform")); Value.IsValid()) {
            if (!DecodeStruct(TBaseStructure<FTransform>::Get(), Value, &Offset, &Blueprint, OutError)) return FailElementField(TEXT("initial_offset_transform"));
            const auto* CapturedOffset = CapturedOffsets.Find(Key);
            if (!CapturedOffset || !CapturedOffset->Equals(Offset)) {
              Hierarchy->SetControlOffsetTransform(Key, Offset, true, false, false, false);
              Hierarchy->SetControlOffsetTransform(Key, Offset, false, false, false, false);
            }
          }
          if (const auto Value = Element->TryGetField(TEXT("initial_shape_transform")); Value.IsValid()) {
            if (!DecodeStruct(TBaseStructure<FTransform>::Get(), Value, &Shape, &Blueprint, OutError)) return FailElementField(TEXT("initial_shape_transform"));
            const auto* CapturedShape = CapturedShapes.Find(Key);
            if (!CapturedShape || !CapturedShape->Equals(Shape)) {
              Hierarchy->SetControlShapeTransform(Key, Shape, true, false);
              Hierarchy->SetControlShapeTransform(Key, Shape, false, false);
            }
          }
        }
      }
      if (auto* Socket = Hierarchy->Find<FRigSocketElement>(Key)) {
        if (const auto Value = Element->TryGetField(TEXT("color")); Value.IsValid()) {
          FLinearColor Color = Socket->GetColor(Hierarchy);
          if (!DecodeStruct(TBaseStructure<FLinearColor>::Get(), Value, &Color, &Blueprint, OutError)) return FailElementField(TEXT("color"));
          if (Color != Socket->GetColor(Hierarchy)) Socket->SetColor(Color, Hierarchy);
        }
        if (Element->HasField(TEXT("description"))) {
          FString Description;
          if (!Element->TryGetStringField(TEXT("description"), Description)) {
            OutError = InputFailure(TEXT("rigging_description_expected_string"), TEXT("Description must be a string."),
                *Element, TEXT("/description"), Element->TryGetField(TEXT("description")),
                MakeShared<FJsonValueString>(TEXT("string")), UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint));
            UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(OutError,
                TEXT("/hierarchy/elements/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Pair.Key));
            return false;
          }
          if (Description != Socket->GetDescription(Hierarchy)) Socket->SetDescription(Description, Hierarchy);
        }
      }
      if (Hierarchy->GetIndex(Key) == INDEX_NONE
          || ((!bExisting || bParentEdited) && Hierarchy->GetFirstParent(Key) != Parent)) {
        const bool bMissing = Hierarchy->GetIndex(Key) == INDEX_NONE;
        const FRigElementKey NativeParent = bMissing ? FRigElementKey() : Hierarchy->GetFirstParent(Key);
        const TSharedPtr<FJsonValue> Expected = bMissing ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(ElementId(Key)))
            : Parent.IsValid() ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(ElementId(Parent))) : MakeShared<FJsonValueNull>();
        const TSharedPtr<FJsonValue> Actual = !bMissing && NativeParent.IsValid()
            ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(ElementId(NativeParent))) : MakeShared<FJsonValueNull>();
        OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("control_rig_hierarchy_intent_mismatch"),
            bMissing ? TEXT("identity") : TEXT("parent"), Expected, Actual, TEXT("hierarchy_apply"),
            bMissing ? TEXT("The native hierarchy did not create the requested element.")
                : TEXT("The native hierarchy did not apply the requested parent."));
        OutError.SourceKind = TEXT("native_result");
        OutError.TargetPath = UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint);
        OutError.SemanticPointer = TEXT("/hierarchy/elements/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Pair.Key);
        return false;
      }
      Applied.Add(Pair.Key);
      ++OutStats.HierarchyElements;
    }
    if (Applied.Num() == Before) {
      OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("control_rig_hierarchy_parent_cycle_or_missing_parent"),
          TEXT("The remaining hierarchy elements have unresolved parent dependencies."), TEXT("hierarchy_apply"));
      OutError.TargetPath = UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint);
      for (const auto& Pending : (*Elements)->Values) {
        if (Applied.Contains(Pending.Key)) continue;
        const auto PendingElement = Pending.Value->AsObject();
        auto Error = InputFailure(TEXT("control_rig_hierarchy_parent_unresolved"),
            TEXT("This parent is unavailable; the remaining dependencies contain a missing parent or a cycle."),
            *HierarchyObject, TEXT("/elements/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Pending.Key) + TEXT("/parent"),
            PendingElement->TryGetField(TEXT("parent")),
            MakeShared<FJsonValueString>(TEXT("existing parent or acyclic dependency on another declared element")), UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint));
        UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(Error, TEXT("/hierarchy"));
        OutError.Diagnostics.Add(MoveTemp(Error));
      }
      return false;
    }
  }
  return true;
}

bool ApplyControlRigGraphs(UControlRigBlueprint &Blueprint,
                           const TSharedPtr<const FJsonObject> &GraphsObject,
                           FMutationStats &OutStats, UE::UAI::Contract::FDiagnostic& OutError,
                           const FControlRigGraphEditContexts& GraphContexts) {
  if (!GraphsObject.IsValid())
    return true;
  const FString LogicalTarget = UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint);
  const auto FailInput = [&](const TCHAR* Code, const TCHAR* Reason, const FString& Pointer,
      const TSharedPtr<FJsonValue>& Actual, const FString& Expected) {
    OutError = InputFailure(Code, Reason, *GraphsObject, Pointer, Actual,
        MakeShared<FJsonValueString>(Expected), LogicalTarget);
    if (OutError.SourceKind != TEXT("command_input"))
      UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(OutError, TEXT("/graphs"));
    return false;
  };
  const auto FailNative = [&](const TCHAR* Code, const TCHAR* Reason, const FString& Pointer,
      const FString& Property, const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonValue>& Expected) {
    OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Property, Expected, Actual, TEXT("graph_apply"), Reason);
    DescribeNativeProjectionFailure(OutError, LogicalTarget, Pointer);
    return false;
  };
  const auto QualifyNodeFailure = [&](const FJsonObject& Node, const FString& Pointer, const FString& Field = FString()) {
    if (OutError.SourceKind != TEXT("command_input"))
      UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(OutError, Field.IsEmpty() ? FString() : TEXT("/") + Field);
    UeAgentAuthoringDiagnostics::LocateFailure(OutError, Node, TEXT("graph_apply"), LogicalTarget, Field);
    if (OutError.SourceKind == TEXT("native_result")) {
      if (OutError.SemanticPointer.IsEmpty()) OutError.SemanticPointer = Pointer;
    } else if (OutError.SourceKind != TEXT("command_input")) {
      UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(OutError, Pointer);
    }
    return false;
  };
  if (!ValidateFields(GraphsObject, {TEXT("position_space"), TEXT("graphs")},
                      TEXT("control_rig_graphs"), OutError, UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint), TEXT("/graphs")))
    return false;
  if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(*GraphsObject,
      {{TEXT("position_space"), EJson::String}, {TEXT("graphs"), EJson::Object}}, OutError, TEXT("/graphs"))) return false;
  FString Space;
  if (GraphsObject->TryGetStringField(TEXT("position_space"), Space) &&
      Space != TEXT("rigvm_graph_canvas_2d")) {
    return FailInput(TEXT("control_rig_graph_position_space_invalid"),
        TEXT("Graph node positions must use RigVM graph canvas coordinates."), TEXT("/position_space"),
        MakeShared<FJsonValueString>(Space), TEXT("rigvm_graph_canvas_2d"));
  }
  const TSharedPtr<FJsonObject> *Graphs = nullptr;
  if (!GraphsObject->TryGetObjectField(TEXT("graphs"), Graphs))
    return true;
  if (!Graphs || !Graphs->IsValid()) {
    return FailInput(TEXT("control_rig_graphs_invalid"), TEXT("Graphs must be an object keyed by graph identity."),
        TEXT("/graphs"), GraphsObject->TryGetField(TEXT("graphs")), TEXT("object"));
  }
  TArray<TSharedPtr<FJsonValue>> LegacyGraphs;
  TSet<const URigVMNode*> AddedNodes;
  TMap<FString, FString> LegacySourcePointers;
  LegacySourcePointers.Add(TEXT("/"), TEXT("/graphs"));
  for (const TPair<FString, TSharedPtr<FJsonValue>> &GraphPair :
       (*Graphs)->Values) {
    const TSharedPtr<FJsonObject> Graph =
        GraphPair.Value.IsValid() && GraphPair.Value->Type == EJson::Object ? GraphPair.Value->AsObject() : nullptr;
    const FString RelativeGraphPointer = TEXT("/graphs/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(GraphPair.Key);
    if (!Graph.IsValid())
      return FailInput(TEXT("control_rig_graph_invalid"), TEXT("Each graph must be an object."),
          RelativeGraphPointer, GraphPair.Value, TEXT("object"));
    if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(*Graph,
        {{TEXT("nodes"), EJson::Object}, {TEXT("links"), EJson::Array}}, OutError,
        TEXT("/graphs/graphs/") + GraphPair.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")))) return false;
    const FString LegacyGraphPath = FString::Printf(TEXT("/graphs/%d"), LegacyGraphs.Num());
    const FString GraphPointer = TEXT("/graphs/graphs/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(GraphPair.Key);
    const auto* GraphContext = GraphContexts.Find(GraphPair.Key);
    const auto* Connections = GraphContext ? &GraphContext->Connections : nullptr;
    LegacySourcePointers.Add(LegacyGraphPath, GraphPointer);
    URigVMGraph *NativeGraph = nullptr;
    for (URigVMGraph *Candidate : Blueprint.GetAllModels()) {
      if (Candidate && Candidate->GetNodePath() == GraphPair.Key) {
        NativeGraph = Candidate;
        break;
      }
    }
    if (!NativeGraph) {
      return FailNative(TEXT("control_rig_graph_not_found"), TEXT("The Blueprint has no native graph matching this identity."),
          GraphPointer, TEXT("graph"), MakeShared<FJsonValueString>(GraphPair.Key),
          MakeShared<FJsonValueString>(TEXT("existing native graph")));
    }
    using namespace UE::UAI::HjsonWorkspace;
    TArray<FGraphAuthoringNodeAliasInput> NodeAliasInputs;
    for (URigVMNode *Node : NativeGraph->GetNodes()) {
      if (Node) NodeAliasInputs.Add({Node->GetName(), Node->GetNodeTitle()});
    }
    FGraphAuthoringAliasProjection NodeAliases;
    FWorkspaceError AliasError;
    if (!TryBuildGraphAuthoringAliasProjection(NodeAliasInputs, NodeAliases, AliasError)) {
      OutError = AliasError.GetDiagnostic();
      DescribeNativeProjectionFailure(OutError, UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint),
          GraphPointer);
      return false;
    }
    // Rebuilding/removing a node can change title-derived aliases. Retain the
    // captured identities for this edit; the subsequent export chooses new aliases.
    if (GraphContext) {
      NodeAliases = GraphContext->NodeAliases;
      for (auto It = NodeAliases.SourceIdByAlias.CreateIterator(); It; ++It)
        if (!NativeGraph->FindNodeByName(FName(*It.Value()))) {
          NodeAliases.AliasBySourceId.Remove(It.Value());
          It.RemoveCurrent();
        }
    }
    const TSharedPtr<FJsonObject> *Nodes = nullptr;
    Graph->TryGetObjectField(TEXT("nodes"), Nodes);
    if (Nodes && Nodes->IsValid()) {
      for (const auto &NodePair : (*Nodes)->Values) {
        if (NodeAliases.SourceIdByAlias.Contains(NodePair.Key))
          continue;
        const TSharedPtr<FJsonObject> Node =
            NodePair.Value.IsValid() && NodePair.Value->Type == EJson::Object
                ? NodePair.Value->AsObject() : nullptr;
        const FString NodePointer = GraphPointer + TEXT("/nodes/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(NodePair.Key);
        const FString RelativeNodePointer = RelativeGraphPointer + TEXT("/nodes/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(NodePair.Key);
        const TSharedPtr<FJsonObject> *Candidate = nullptr;
        FString CandidateName;
        UE::UAI::HjsonWorkspace::DomainAuthoring::FDomainHjsonCandidateSelector CandidateSelector;
        const bool bCandidate = Node.IsValid() && Node->HasField(TEXT("candidate"));
        if (!Node.IsValid() || (bCandidate &&
            (!Node->TryGetObjectField(TEXT("candidate"), Candidate) ||
             !Candidate || !Candidate->IsValid() ||
             !(*Candidate)->TryGetStringField(TEXT("name"), CandidateName)))) {
          if (!Node.IsValid())
            return FailInput(TEXT("control_rig_graph_new_node_definition_invalid"), TEXT("A new node definition must be an object."),
                RelativeNodePointer, NodePair.Value, TEXT("node definition object"));
          const bool bCandidateObject = Candidate && Candidate->IsValid();
          return FailInput(TEXT("control_rig_graph_new_node_definition_invalid"),
              bCandidateObject ? TEXT("The candidate name must be a string.") : TEXT("The candidate selector must be an object."),
              RelativeNodePointer + (bCandidateObject ? TEXT("/candidate/name") : TEXT("/candidate")),
              bCandidateObject ? (*Candidate)->TryGetField(TEXT("name")) : Node->TryGetField(TEXT("candidate")),
              bCandidateObject ? TEXT("string") : TEXT("candidate selector object"));
        }
        if (bCandidate) {
          UE::UAI::HjsonWorkspace::FWorkspaceError SelectorError;
          if (!UE::UAI::HjsonWorkspace::DomainAuthoring::TryReadCandidateSelector(
                  *Candidate, TEXT("candidate"), CandidateSelector, SelectorError)) {
            OutError = SelectorError.GetDiagnostic();
            UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Node, TEXT("candidate"));
            if (OutError.SourceKind != TEXT("command_input"))
              UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(OutError, NodePointer);
            return false;
          }
        }
        if (bCandidate && NativeGraph != Blueprint.GetDefaultModel()) {
          const auto* DefaultGraph = Blueprint.GetDefaultModel();
          return FailInput(TEXT("control_rig_candidate_requires_default_graph"),
              TEXT("Candidate actions can create nodes only in the Blueprint's default RigVM graph."),
              RelativeNodePointer + TEXT("/candidate"), MakeShared<FJsonValueString>(GraphPair.Key),
              DefaultGraph ? DefaultGraph->GetNodePath() : TEXT("available default RigVM graph"));
        }
        FVector2D Position = FVector2D::ZeroVector;
        if (const auto Value = Node->TryGetField(TEXT("position"));
            Value.IsValid() && !DecodeStruct(TBaseStructure<FVector2D>::Get(),
                                            Value, &Position, &Blueprint, OutError)) {
          return QualifyNodeFailure(*Node, NodePointer, TEXT("position"));
        }
        using namespace UE::UAI::QueryOwners::AnimationRigGraph;
        URigVMNode *Added = nullptr;
        FAnimationRigGraphCandidate ResolvedCandidate;
        FAnimationRigGraphCandidateQueryError CandidateError;
        bool bCreated = false;
        if (bCandidate) bCreated = TryPerformAnimationRigGraphCandidate(Blueprint, CandidateName,
                                                  FVector2f(Position), Added,
                                                  ResolvedCandidate, CandidateError, CandidateSelector.Select);
        else if (URigVMController* Controller = Blueprint.GetController(NativeGraph)) {
          Added = UeAgentRigVMNodeAuthoring::CreateNode(Blueprint, *Controller, *Node, NodePair.Key, Position, OutError);
          bCreated = Added != nullptr;
          if (!bCreated) {
            return QualifyNodeFailure(*Node, NodePointer);
          }
        }
        if (!bCandidate && !bCreated) {
          return FailNative(TEXT("control_rig_graph_controller_missing"), TEXT("The native graph has no controller available to create the node."),
              NodePointer, TEXT("controller_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
        }
        if (!bCreated) {
          auto Failure = CandidateError;
          if (Failure.IsEmpty()) {
            Failure = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_rig_graph_candidate_native_action_create_failed"),
                TEXT("The native RigVM candidate action failed without providing a diagnostic."), TEXT("candidate_create"));
            Failure.SourceKind = TEXT("native_result");
            Failure.MutationMayHaveStarted = true;
          }
          if (Failure.Stage.IsEmpty()) Failure.Stage = TEXT("candidate_create");
          if (Failure.CandidateName.IsEmpty()) Failure.CandidateName = CandidateName;
          // The candidate API's x/y parameters are generated from one encoded position value.
          // Keep native/query failures intact; only that explicit coordinate conversion maps to input.
          if (Failure.SourceKind != TEXT("native_result")
              && (Failure.Source.JsonPath == TEXT("/x") || Failure.Source.JsonPath == TEXT("/y"))) {
            Failure.Source.InternalJsonPath = TEXT("/position");
            Failure.Source.JsonPath = TEXT("/position");
            UeAgentAuthoringDiagnostics::LocateFailure(Failure, *Node, TEXT("candidate_create"), LogicalTarget, TEXT("position"));
            if (Failure.SourceKind != TEXT("command_input"))
              UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(Failure, NodePointer);
          }
          OutError = UeAgentAuthoringDiagnostics::CandidateFailure(
              Failure.Code, CandidateName, LogicalTarget,
              TEXT("the default RigVM graph"), TEXT("animation_rig_graph.candidates"),
              Failure);
          return false;
        }
        AddedNodes.Add(Added);
        NodeAliases.SourceIdByAlias.Add(NodePair.Key, Added->GetName());
        NodeAliases.AliasBySourceId.Add(Added->GetName(), NodePair.Key);
      }
    }
    TMap<FString, FString> VisibleEndpointByNative;
    for (URigVMNode *Node : NativeGraph->GetNodes()) {
      if (!Node) continue;
      const FString NodeAlias = NodeAliases.AliasBySourceId.FindRef(Node->GetName());
      TArray<FGraphAuthoringPinAliasInput> PinAliasInputs;
      TMap<FString, int32> PinOccurrences;
      for (URigVMPin *Pin : Node->GetAllPinsRecursively()) {
        if (!Pin || Pin->GetDirection() == ERigVMPinDirection::Hidden) continue;
        const EGraphAuthoringEndpointDirection Direction =
          Pin->GetDirection() == ERigVMPinDirection::Output
            ? EGraphAuthoringEndpointDirection::Output
            : EGraphAuthoringEndpointDirection::Input;
        const FString OccurrenceKey = Pin->GetName() + TEXT("\n")
          + (Direction == EGraphAuthoringEndpointDirection::Output ? TEXT("output") : TEXT("input"));
        int32& Occurrence = PinOccurrences.FindOrAdd(OccurrenceKey);
        PinAliasInputs.Add({Pin->GetPinPath(false), Pin->GetName(), Occurrence, Direction});
        ++Occurrence;
      }
      FGraphAuthoringPinAliasProjection PinAliases;
      if (!TryBuildGraphAuthoringPinAliasProjection(PinAliasInputs, PinAliases, AliasError)) {
        OutError = AliasError.GetDiagnostic();
        DescribeNativeProjectionFailure(OutError, LogicalTarget, GraphPointer);
        return false;
      }
      for (URigVMPin *Pin : Node->GetAllPinsRecursively()) {
        if (!Pin || Pin->GetDirection() == ERigVMPinDirection::Hidden) continue;
        const FString PinAlias = PinAliases.AliasBySourceId.FindRef(Pin->GetPinPath(false));
        if (PinAlias.IsEmpty()) continue;
        const FString PinPath = Pin->GetPinPath(false);
        const FString NativeEndpoint = PinPath.StartsWith(Node->GetName() + TEXT("."))
          ? PinPath : Node->GetName() + TEXT(".") + PinPath;
        FString VisibleEndpoint;
        if (!TryFormatGraphAuthoringEndpoint(
                NodeAlias, PinAlias, 0,
                Pin->GetDirection() == ERigVMPinDirection::Output
                  ? EGraphAuthoringEndpointDirection::Output
                  : EGraphAuthoringEndpointDirection::Input,
                VisibleEndpoint, AliasError)) {
          OutError = AliasError.GetDiagnostic();
          DescribeNativeProjectionFailure(OutError, LogicalTarget, GraphPointer);
          return false;
        }
        VisibleEndpointByNative.Add(NativeEndpoint, VisibleEndpoint);
        VisibleEndpointByNative.Add(PinPath, VisibleEndpoint);
      }
    }
    TSharedRef<FJsonObject> LegacyGraph = MakeShared<FJsonObject>();
    LegacyGraph->SetStringField(TEXT("name"), GraphPair.Key);
    TArray<TSharedPtr<FJsonValue>> LegacyNodes;
    TSet<FString> DesiredNodeNames;
    if (Graph->TryGetObjectField(TEXT("nodes"), Nodes) && Nodes &&
        Nodes->IsValid()) {
      for (const TPair<FString, TSharedPtr<FJsonValue>> &NodePair :
           (*Nodes)->Values) {
        const TSharedPtr<FJsonObject> Node =
            NodePair.Value.IsValid() && NodePair.Value->Type == EJson::Object ? NodePair.Value->AsObject() : nullptr;
        if (!Node.IsValid())
          return FailInput(TEXT("control_rig_graph_node_invalid"), TEXT("Each graph node must be an object."),
              RelativeGraphPointer + TEXT("/nodes/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(NodePair.Key),
              NodePair.Value, TEXT("node definition object"));
        TSharedRef<FJsonObject> LegacyNode = CloneObject(Node);
        const FString LegacyNodePointer = LegacyGraphPath + FString::Printf(TEXT("/nodes/%d"), LegacyNodes.Num());
        const FString NodePointer = GraphPointer + TEXT("/nodes/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(NodePair.Key);
        LegacySourcePointers.Add(LegacyNodePointer, NodePointer);
        LegacyNode->RemoveField(TEXT("authoring_source"));
        const FString *NativeNodeName = NodeAliases.SourceIdByAlias.Find(NodePair.Key);
        if (!NativeNodeName) {
          return FailNative(TEXT("control_rig_graph_node_alias_unknown"), TEXT("The node alias has no native identity after node preparation."),
              NodePointer, TEXT("node_alias"), MakeShared<FJsonValueString>(NodePair.Key),
              MakeShared<FJsonValueString>(TEXT("alias bound to a native node")));
        }
        LegacyNode->SetStringField(TEXT("name"), *NativeNodeName);
        DesiredNodeNames.Add(*NativeNodeName);
        if (Node->HasField(TEXT("candidate"))) {
          const URigVMNode *Created = NativeGraph->FindNodeByName(FName(**NativeNodeName));
          if (!Created) {
            return FailNative(TEXT("control_rig_candidate_node_missing"), TEXT("The node created for this candidate is no longer present in the native graph."),
                NodePointer, TEXT("node_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
          }
          LegacyNode->RemoveField(TEXT("candidate"));
          FJsonObject NativeDefinition;
          UeAgentRigVMNodeAuthoring::CaptureDefinition(Blueprint, *Created, NativeDefinition);
          for (const auto& Field : NativeDefinition.Values)
            if (!LegacyNode->HasField(Field.Key)) LegacyNode->SetField(Field.Key, Field.Value);
        }
        const TSharedPtr<FJsonObject> *PinDefaults = nullptr;
        if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(*Node,
            {{TEXT("pin_defaults"), EJson::Object}}, OutError)) return QualifyNodeFailure(*Node, NodePointer);
        if (Node->TryGetObjectField(TEXT("pin_defaults"), PinDefaults) && PinDefaults && PinDefaults->IsValid()) {
          TSharedRef<FJsonObject> NativeDefaults = MakeShared<FJsonObject>();
          for (const TPair<FString, TSharedPtr<FJsonValue>> &DefaultPair : (*PinDefaults)->Values) {
            URigVMNode *NativeNode = nullptr;
            for (URigVMNode *CandidateNode : NativeGraph->GetNodes()) {
              if (CandidateNode && CandidateNode->GetName() == *NativeNodeName) {
                NativeNode = CandidateNode;
                break;
              }
            }
            if (!NativeNode) {
              return FailNative(TEXT("control_rig_graph_node_not_found"), TEXT("The native graph node is no longer available while applying its pin defaults."),
                  NodePointer, TEXT("node_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
            }
            TArray<FGraphAuthoringPinAliasInput> PinAliasInputs;
            TMap<FString, int32> PinOccurrences;
            for (URigVMPin *Pin : NativeNode->GetAllPinsRecursively()) {
              if (!Pin || Pin->GetDirection() == ERigVMPinDirection::Hidden) continue;
              const EGraphAuthoringEndpointDirection Direction = Pin->GetDirection() == ERigVMPinDirection::Output
                ? EGraphAuthoringEndpointDirection::Output : EGraphAuthoringEndpointDirection::Input;
              const FString Key = Pin->GetName() + TEXT("\n") + (Direction == EGraphAuthoringEndpointDirection::Output ? TEXT("output") : TEXT("input"));
              int32& Occurrence = PinOccurrences.FindOrAdd(Key);
              PinAliasInputs.Add({Pin->GetPinPath(false), Pin->GetName(), Occurrence, Direction});
              ++Occurrence;
            }
            FGraphAuthoringPinAliasProjection PinAliases;
            if (!TryBuildGraphAuthoringPinAliasProjection(PinAliasInputs, PinAliases, AliasError)) {
              OutError = AliasError.GetDiagnostic();
              DescribeNativeProjectionFailure(OutError, LogicalTarget, NodePointer);
              return false;
            }
            const FString *NativePinPath = PinAliases.SourceIdByAlias.Find(DefaultPair.Key);
            if (!NativePinPath) {
              return FailInput(TEXT("control_rig_graph_pin_alias_unknown"), TEXT("The requested pin alias does not exist on this node."),
                  RelativeGraphPointer + TEXT("/nodes/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(NodePair.Key)
                      + TEXT("/pin_defaults/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(DefaultPair.Key),
                  MakeShared<FJsonValueString>(DefaultPair.Key), TEXT("existing pin alias"));
            }
            NativeDefaults->SetField(*NativePinPath, DefaultPair.Value);
            LegacySourcePointers.Add(LegacyNodePointer + TEXT("/pin_defaults/")
                + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(*NativePinPath),
                NodePointer + TEXT("/pin_defaults/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(DefaultPair.Key));
          }
          LegacyNode->SetObjectField(TEXT("pin_defaults"), NativeDefaults);
        }
        LegacyNode->RemoveField(TEXT("pin_interface"));
        const TSharedPtr<FJsonValue> PositionValue =
            Node->TryGetField(TEXT("position"));
        if (PositionValue.IsValid()) {
          FVector2D Position = FVector2D::ZeroVector;
          if (!DecodeStruct(TBaseStructure<FVector2D>::Get(), PositionValue,
                            &Position, &Blueprint, OutError)) {
            return QualifyNodeFailure(*Node, NodePointer, TEXT("position"));
          }
          TSharedRef<FJsonObject> LegacyPosition = MakeShared<FJsonObject>();
          LegacyPosition->SetNumberField(TEXT("x"), Position.X);
          LegacyPosition->SetNumberField(TEXT("y"), Position.Y);
          // x/y are generated storage fields; a rejection belongs to the original position value.
          LegacySourcePointers.Add(LegacyNodePointer + TEXT("/position/x"), NodePointer + TEXT("/position"));
          LegacySourcePointers.Add(LegacyNodePointer + TEXT("/position/y"), NodePointer + TEXT("/position"));
          LegacyNode->SetObjectField(TEXT("position"), LegacyPosition);
        }
        LegacyNodes.Add(MakeShared<FJsonValueObject>(LegacyNode));
        ++OutStats.GraphNodes;
      }
    }
    LegacyGraph->SetArrayField(TEXT("nodes"), LegacyNodes);
    TArray<TSharedPtr<FJsonValue>> LegacyLinks;
    TSet<FString> DesiredLinks;
    const TArray<TSharedPtr<FJsonValue>> *Links = nullptr;
    if (Graph->TryGetArrayField(TEXT("links"), Links) && Links) {
      int32 SourceLinkIndex = 0;
      for (const TSharedPtr<FJsonValue> &LinkValue : *Links) {
        const int32 OriginalLinkIndex = SourceLinkIndex++;
        const TArray<TSharedPtr<FJsonValue>> *LinkPair = nullptr;
        if (!LinkValue.IsValid() || !LinkValue->TryGetArray(LinkPair) ||
            !LinkPair || LinkPair->Num() != 2 || !(*LinkPair)[0].IsValid() ||
            (*LinkPair)[0]->Type != EJson::String ||
            !(*LinkPair)[1].IsValid() ||
            (*LinkPair)[1]->Type != EJson::String) {
          const bool bPair = LinkPair && LinkPair->Num() == 2;
          const int32 Endpoint = bPair && (*LinkPair)[0].IsValid() && (*LinkPair)[0]->Type == EJson::String ? 1 : 0;
          return FailInput(TEXT("control_rig_graph_link_invalid"),
              bPair ? TEXT("A link endpoint alias must be a string.") : TEXT("A graph link must contain exactly two endpoint aliases."),
              RelativeGraphPointer + FString::Printf(TEXT("/links/%d"), OriginalLinkIndex)
                  + (bPair ? FString::Printf(TEXT("/%d"), Endpoint) : FString()),
              bPair ? (*LinkPair)[Endpoint] : LinkValue,
              bPair ? TEXT("string endpoint alias") : TEXT("array of two string endpoint aliases"));
        }
        TSharedRef<FJsonObject> LegacyLink = MakeShared<FJsonObject>();
        if (Connections && Connections->IsUnchanged((*LinkPair)[0]->AsString(), (*LinkPair)[1]->AsString())) continue;
        const FString *NativeSource = nullptr;
        const FString *NativeTarget = nullptr;
        for (const TPair<FString, FString> &Mapping : VisibleEndpointByNative) {
          if (Mapping.Value == (*LinkPair)[0]->AsString()) { NativeSource = &Mapping.Key; break; }
        }
        for (const TPair<FString, FString> &Mapping : VisibleEndpointByNative) {
          if (Mapping.Value == (*LinkPair)[1]->AsString()) { NativeTarget = &Mapping.Key; break; }
        }
        if (!NativeSource || !NativeTarget) {
          const int32 Endpoint = NativeSource ? 1 : 0;
          return FailInput(TEXT("control_rig_graph_endpoint_alias_unknown"), TEXT("The link endpoint alias does not identify a visible graph pin."),
              RelativeGraphPointer + FString::Printf(TEXT("/links/%d/%d"), OriginalLinkIndex, Endpoint),
              (*LinkPair)[Endpoint], TEXT("visible graph endpoint alias"));
        }
        LegacyLink->SetStringField(TEXT("source"), *NativeSource);
        LegacyLink->SetStringField(TEXT("target"), *NativeTarget);
        DesiredLinks.Add(*NativeSource + TEXT("\n") + *NativeTarget);
        const FString LegacyLinkPointer = LegacyGraphPath + FString::Printf(TEXT("/links/%d"), LegacyLinks.Num());
        const FString LinkPointer = GraphPointer + FString::Printf(TEXT("/links/%d"), OriginalLinkIndex);
        LegacySourcePointers.Add(LegacyLinkPointer, LinkPointer);
        LegacySourcePointers.Add(LegacyLinkPointer + TEXT("/source"), LinkPointer + TEXT("/0"));
        LegacySourcePointers.Add(LegacyLinkPointer + TEXT("/target"), LinkPointer + TEXT("/1"));
        LegacyLinks.Add(MakeShared<FJsonValueObject>(LegacyLink));
        ++OutStats.GraphLinks;
      }
    }
    LegacyGraph->SetArrayField(TEXT("links"), LegacyLinks);
    URigVMController *Controller = Blueprint.GetController(NativeGraph);
    if (!Controller) {
      return FailNative(TEXT("control_rig_graph_controller_missing"), TEXT("The native graph has no controller available for node and connection edits."),
          GraphPointer, TEXT("controller_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
    }
    const TArray<URigVMLink *> ExistingLinks = NativeGraph->GetLinks();
    for (URigVMLink *Link : ExistingLinks) {
      if (Connections && Connections->bHasResolvedIntent) break;
      if (Link && !DesiredLinks.Contains(Link->GetSourcePinPath() + TEXT("\n") + Link->GetTargetPinPath()) &&
          !Controller->BreakLink(Link->GetSourcePinPath(), Link->GetTargetPinPath(), false, false)) {
        TArray<TSharedPtr<FJsonValue>> Endpoints{MakeShared<FJsonValueString>(Link->GetSourcePinPath()), MakeShared<FJsonValueString>(Link->GetTargetPinPath())};
        return FailNative(TEXT("control_rig_graph_remove_link_failed"), TEXT("The native controller refused to remove this connection."),
            GraphPointer + TEXT("/links"), TEXT("connection"), MakeShared<FJsonValueArray>(MoveTemp(Endpoints)), MakeShared<FJsonValueNull>());
      }
    }
    TArray<URigVMNode *> RemovedNodes;
    for (URigVMNode *Node : NativeGraph->GetNodes())
      if (Node && !DesiredNodeNames.Contains(Node->GetName()))
        RemovedNodes.Add(Node);
    if (!RemovedNodes.IsEmpty() && !Controller->RemoveNodes(RemovedNodes, false, false)) {
      auto Actual = MakeShared<FJsonObject>();
      TArray<TSharedPtr<FJsonValue>> Names;
      for (const auto* Node : RemovedNodes) Names.Add(MakeShared<FJsonValueString>(Node->GetName()));
      Actual->SetArrayField(TEXT("requested_nodes"), MoveTemp(Names));
      Actual->SetBoolField(TEXT("remove_succeeded"), false);
      auto Expected = MakeShared<FJsonObject>();
      Expected->SetBoolField(TEXT("remove_succeeded"), true);
      return FailNative(TEXT("control_rig_graph_remove_nodes_failed"), TEXT("The native controller did not complete the requested node removals."),
          GraphPointer + TEXT("/nodes"), TEXT("node_removal"), MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueObject>(Expected));
    }
    LegacyGraphs.Add(MakeShared<FJsonValueObject>(LegacyGraph));
  }
  TSharedRef<FJsonObject> LegacyRoot = MakeShared<FJsonObject>();
  // Omitted nodes/links were removed above. Reuse surviving and native-created
  // nodes while the existing backend applies their fields and desired links.
  LegacyRoot->SetBoolField(TEXT("replace_nodes"), false);
  LegacyRoot->SetArrayField(TEXT("graphs"), LegacyGraphs);
  TArray<UE::UAI::Contract::FDiagnostic> Issues;
  if (!UeAgentControlRigOps::ApplyGraphsJson(
          &Blueprint, LegacyRoot, Issues, &AddedNodes)) {
    const auto RemapPointer = [&](FString& Pointer) {
      const TPair<FString, FString>* Match = nullptr;
      for (const auto& Entry : LegacySourcePointers)
        if ((Pointer == Entry.Key || Pointer.StartsWith(Entry.Key + TEXT("/")))
            && (!Match || Entry.Key.Len() > Match->Key.Len())) Match = &Entry;
      if (Match) Pointer = Match->Value + Pointer.Mid(Match->Key.Len());
    };
    TFunction<void(UE::UAI::Contract::FDiagnostic&)> LocateGenerated = [&](auto& Error) {
      if (Error.SourceKind != TEXT("command_input")) {
        if (Error.SourceKind != TEXT("native_result") && Error.Source.SourceFile.IsEmpty()) {
          RemapPointer(Error.Source.JsonPath);
          RemapPointer(Error.Source.InternalJsonPath);
        }
        // A native observation can identify the requested node/link without claiming a source-file error.
        if (Error.Source.SourceFile.IsEmpty()) RemapPointer(Error.SemanticPointer);
      }
      for (auto& Child : Error.Diagnostics) LocateGenerated(Child);
      if (Error.PublicCause) {
        auto Cause = MakeShared<UE::UAI::Contract::FDiagnostic>(*Error.PublicCause);
        LocateGenerated(*Cause);
        Error.PublicCause = MoveTemp(Cause);
      }
    };
    UeAgentAuthoringDiagnostics::FDiagnostic Failures;
    for (auto Failure : Issues) {
      if (Failure.IsEmpty() || Failure.Severity == UE::UAI::Contract::ESeverity::Info || Failure.Code == TEXT("json_unknown_field")) continue;
      if (Failure.Stage.IsEmpty()) Failure.Stage = TEXT("graph_apply");
      if (Failure.TargetPath.IsEmpty()) Failure.TargetPath = UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint);
      LocateGenerated(Failure);
      if (Failure.Code == TEXT("graph_connection_intent_not_retained"))
        UeAgentAuthoringDiagnostics::ReportConsistencyWarning(Failure);
      Failures.Append(Failure);
    }
    OutError = Failures.Code.IsEmpty() ? UE::UAI::Contract::FDiagnostic::Failure(TEXT("control_rig_graph_apply_failed"), TEXT("The Control Rig graph could not be applied."), TEXT("graph_apply")) : MoveTemp(Failures);
    return false;
  }
  return true;
}

bool ApplyControlRigDocument(UControlRigBlueprint &Blueprint,
                             const TSharedPtr<const FJsonObject> &InputDocument,
                             FMutationStats &OutStats, UE::UAI::Contract::FDiagnostic& OutError,
                             const TSharedPtr<const FJsonObject>& CapturedDocument) {
  FRigVMBlueprintCompileScope CompileScope(&Blueprint);
  const auto Document = CloneObject(ConstCastSharedPtr<FJsonObject>(InputDocument));
  if (!ValidateFields(InputDocument,
                      {TEXT("rig_type"), TEXT("preview_mesh"),
                       TEXT("shape_libraries"), TEXT("hierarchy"),
                       TEXT("graphs"), TEXT("variables")},
                      TEXT("control_rig_document"), OutError, UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint)))
    return false;
  FString PreviewMesh;
  if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(*Document,
      {{TEXT("rig_type"), EJson::String}, {TEXT("preview_mesh"), EJson::String, true},
       {TEXT("shape_libraries"), EJson::Array}, {TEXT("hierarchy"), EJson::Object},
       {TEXT("variables"), EJson::Object}, {TEXT("graphs"), EJson::Object}}, OutError)) return false;
  FString RequestedRigType;
  if (Document->TryGetStringField(TEXT("rig_type"), RequestedRigType)
      && RequestedRigType != ControlRigTypeName(Blueprint)) {
    UeAgentAuthoringDiagnostics::FDiagnostic Diagnostic;
    Diagnostic.Code = TEXT("control_rig_type_is_native_state");
    Diagnostic.Stage = TEXT("document_prepare");
    Diagnostic.TargetPath = UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint);
    Diagnostic.PropertyPath = TEXT("rig_type");
    Diagnostic.Source.JsonPath = TEXT("/rig_type");
    Diagnostic.PublicMessage = TEXT("rig_type reports the native rig class/module state. Changing it requires a native rig conversion, which can rebuild the hierarchy and graphs; it is not a document property assignment.");
    Diagnostic.ActualValue = MakeShared<FJsonValueString>(RequestedRigType);
    Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(ControlRigTypeName(Blueprint));
    Diagnostic.Source.InternalJsonPath = TEXT("/rig_type");
    Diagnostic.SourceKind = TEXT("authored_value");
    UeAgentAuthoringDiagnostics::LocateAuthoringSource(Diagnostic, *InputDocument);
    OutError = MoveTemp(Diagnostic);
    return false;
  }
  FControlRigGraphEditContexts GraphContexts;
  const TSharedPtr<FJsonObject>* DesiredGraphDocument = nullptr;
  const TSharedPtr<FJsonObject>* CapturedGraphDocument = nullptr;
  const TSharedPtr<FJsonObject>* DesiredGraphs = nullptr;
  const TSharedPtr<FJsonObject>* CapturedGraphs = nullptr;
  if (CapturedDocument && Document->TryGetObjectField(TEXT("graphs"), DesiredGraphDocument)
      && CapturedDocument->TryGetObjectField(TEXT("graphs"), CapturedGraphDocument)
      && (*DesiredGraphDocument)->TryGetObjectField(TEXT("graphs"), DesiredGraphs)
      && (*CapturedGraphDocument)->TryGetObjectField(TEXT("graphs"), CapturedGraphs)) {
    FControlRigNativeLinks NativeLinks;
    TSharedPtr<FJsonObject> NativeDocument;
    if (!BuildControlRigDocument(Blueprint, NativeDocument, OutError, &NativeLinks, &GraphContexts)) return false;
    for (const auto& GraphPair : (*DesiredGraphs)->Values) {
      const TSharedPtr<FJsonObject>* BeforeGraph = nullptr;
      const auto Graph = GraphPair.Value.IsValid() && GraphPair.Value->Type == EJson::Object
          ? GraphPair.Value->AsObject() : nullptr;
      if (!Graph || !(*CapturedGraphs)->TryGetObjectField(GraphPair.Key, BeforeGraph)) continue;
      auto& GraphContext = GraphContexts.FindOrAdd(GraphPair.Key);
      auto& Context = GraphContext.Connections;
      Context.bHasResolvedIntent = true;
      UE::UAI::HjsonWorkspace::FGraphAuthoringConnectionContext BeforeLinks, RequestedLinks;
      BeforeLinks.bHasResolvedIntent = RequestedLinks.bHasResolvedIntent = true;
      const TArray<TSharedPtr<FJsonValue>>* Links = nullptr;
      const auto CollectLinks = [&](const FJsonObject& Owner,
          UE::UAI::HjsonWorkspace::FGraphAuthoringConnectionContext& Target) {
        if (!Owner.TryGetArrayField(TEXT("links"), Links)) return;
        for (const auto& Value : *Links) {
          const TArray<TSharedPtr<FJsonValue>>* Pair = nullptr;
          if (Value.IsValid() && Value->TryGetArray(Pair) && Pair->Num() == 2
              && (*Pair)[0].IsValid() && (*Pair)[1].IsValid()
              && (*Pair)[0]->Type == EJson::String && (*Pair)[1]->Type == EJson::String)
            Target.UnchangedConnections.Add((*Pair)[0]->AsString() + TEXT("\n") + (*Pair)[1]->AsString());
        }
      };
      CollectLinks(**BeforeGraph, BeforeLinks);
      CollectLinks(*Graph, RequestedLinks);
      Context.UnchangedConnections = BeforeLinks.UnchangedConnections.Intersect(RequestedLinks.UnchangedConnections);
      // Freeze and apply deletions before hierarchy/variable callbacks rebuild pins.
      if (const auto* NativeGraphLinks = NativeLinks.Find(GraphPair.Key))
        for (URigVMGraph* NativeGraph : Blueprint.GetAllModels()) {
          if (!NativeGraph || NativeGraph->GetNodePath() != GraphPair.Key) continue;
          auto* Controller = Blueprint.GetController(NativeGraph);
          if (!Controller) {
            OutError = NativeDocumentFailure(TEXT("control_rig_graph_controller_missing"), TEXT("The native graph controller is unavailable while applying link removals."),
                UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint),
                TEXT("/graphs/graphs/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(GraphPair.Key),
                MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
            return false;
          }
          for (const auto& Link : *NativeGraphLinks)
            if (!RequestedLinks.UnchangedConnections.Contains(Link.Key)) {
              GraphContext.RemovedLinks.Add(Link.Value);
              if (!Controller->BreakLink(Link.Value.Key, Link.Value.Value, false, false)) {
                TArray<TSharedPtr<FJsonValue>> Endpoints{MakeShared<FJsonValueString>(Link.Value.Key), MakeShared<FJsonValueString>(Link.Value.Value)};
                OutError = NativeDocumentFailure(TEXT("control_rig_graph_remove_link_failed"), TEXT("The native controller failed to remove the requested graph connection."),
                    UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint),
                    TEXT("/graphs/graphs/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(GraphPair.Key) + TEXT("/links"),
                    MakeShared<FJsonValueArray>(Endpoints), MakeShared<FJsonValueNull>());
                return false;
              }
            }
        }
      const TSharedPtr<FJsonObject>* BeforeNodes = nullptr;
      const TSharedPtr<FJsonObject>* Nodes = nullptr;
      if ((*BeforeGraph)->TryGetObjectField(TEXT("nodes"), BeforeNodes) && Graph->TryGetObjectField(TEXT("nodes"), Nodes))
        for (const auto& Node : (*Nodes)->Values) {
          const TSharedPtr<FJsonObject>* BeforeNode = nullptr;
          if (Node.Value.IsValid() && Node.Value->Type == EJson::Object
              && (*BeforeNodes)->TryGetObjectField(Node.Key, BeforeNode)) {
            auto Before = CloneObject(*BeforeNode);
            auto Desired = CloneObject(Node.Value->AsObject());
            Before->RemoveField(TEXT("authoring_source"));
            Desired->RemoveField(TEXT("authoring_source"));
            using UE::UAI::StateSemantic::CanonicalJson::AreJsonValuesEquivalent;
            if (AreJsonValuesEquivalent(MakeShared<FJsonValueObject>(Before), MakeShared<FJsonValueObject>(Desired)))
              GraphContext.UnchangedNodes.Add(Node.Key);
            auto MutableNode = Node.Value->AsObject();
            UE::UAI::HjsonWorkspace::ProjectGraphAuthoringPinDefaultChanges(**BeforeNode, *MutableNode);
            // Native creation descriptors in an unchanged node are context too.
            for (URigVMGraph* NativeGraph : Blueprint.GetAllModels()) {
              if (!NativeGraph || NativeGraph->GetNodePath() != GraphPair.Key) continue;
              if (URigVMNode* NativeNode = NativeGraph->FindNodeByName(FName(*GraphContext.NodeAliases.SourceIdByAlias.FindRef(Node.Key)))) {
                FJsonObject Definition;
                UeAgentRigVMNodeAuthoring::CaptureDefinition(Blueprint, *NativeNode, Definition);
                for (const auto& Field : Definition.Values)
                  if (AreJsonValuesEquivalent((*BeforeNode)->TryGetField(Field.Key), MutableNode->TryGetField(Field.Key)))
                    MutableNode->RemoveField(Field.Key);
              }
            }
          }
        }
    }
  }
  if (Document->HasField(TEXT("preview_mesh"))) {
    if (!UeAgentAuthoringJsonFields::ReadReferenceField(*Document, TEXT("preview_mesh"), PreviewMesh, OutError)) return false;
    TSharedRef<FJsonObject> Preview = MakeShared<FJsonObject>();
    Preview->SetStringField(TEXT("preview_skeletal_mesh"), PreviewMesh);
    TArray<UE::UAI::Contract::FDiagnostic> Issues;
    if (!UeAgentControlRigOps::ApplyPreviewJson(
            &Blueprint, Preview, TEXT("provider_extension.preview_mesh"),
            Issues)) {
      OutError = UeAgentAuthoringDiagnostics::MakeIssuesFailure(
          TEXT("control_rig_preview_apply_failed"), Issues, TEXT("preview_apply"), UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint));
      UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Document, TEXT("preview_apply"), UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint), TEXT("preview_mesh"));
      return false;
    }
  }
  const TArray<TSharedPtr<FJsonValue>> *ShapeLibraries = nullptr;
  if (Document->TryGetArrayField(TEXT("shape_libraries"), ShapeLibraries) &&
      ShapeLibraries) {
    TSharedRef<FJsonObject> ShapeReferences = MakeShared<FJsonObject>();
    ShapeReferences->SetArrayField(TEXT("shape_libraries"), *ShapeLibraries);
    TArray<UE::UAI::Contract::FDiagnostic> Issues;
    if (!UeAgentControlRigOps::ApplyShapeLibrariesJson(
            &Blueprint, ShapeReferences,
            TEXT("provider_extension.shape_libraries"), Issues)) {
      OutError = UeAgentAuthoringDiagnostics::MakeIssuesFailure(
          TEXT("control_rig_shape_library_apply_failed"), Issues, TEXT("shape_library_apply"), UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint));
      UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Document, TEXT("shape_library_apply"), UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint), TEXT("shape_libraries"));
      return false;
    }
  }
  const TSharedPtr<FJsonObject> *Hierarchy = nullptr;
  if (Document->TryGetObjectField(TEXT("hierarchy"), Hierarchy) &&
      !ApplyControlRigHierarchy(Blueprint, Hierarchy ? *Hierarchy : nullptr,
                                OutStats, OutError)) {
    UeAgentAuthoringDiagnostics::LocateFailure(OutError, *InputDocument,
        TEXT("document_apply"), UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint));
    return false;
  }
  const TSharedPtr<FJsonObject> *Variables = nullptr;
  if (Document->TryGetObjectField(TEXT("variables"), Variables) && Variables &&
      Variables->IsValid()) {
    const TSharedPtr<FJsonObject>* BeforeVariables = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* BeforeRows = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* DesiredRows = nullptr;
    if (CapturedDocument && CapturedDocument->TryGetObjectField(TEXT("variables"), BeforeVariables)
        && (*BeforeVariables)->TryGetArrayField(TEXT("variables"), BeforeRows)
        && (*Variables)->TryGetArrayField(TEXT("variables"), DesiredRows)) {
      for (const auto& Value : *DesiredRows) {
        const auto Desired = Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
        FString Name;
        if (!Desired || !Desired->TryGetStringField(TEXT("name"), Name)) continue;
        for (const auto& BeforeValue : *BeforeRows) {
          const auto Before = BeforeValue.IsValid() && BeforeValue->Type == EJson::Object ? BeforeValue->AsObject() : nullptr;
          FString BeforeName;
          if (Before && Before->TryGetStringField(TEXT("name"), BeforeName) && BeforeName == Name
              && UE::UAI::StateSemantic::CanonicalJson::AreJsonValuesEquivalent(
                  Before->TryGetField(TEXT("default_property")), Desired->TryGetField(TEXT("default_property")))) {
            Desired->RemoveField(TEXT("default_property"));
            break;
          }
        }
      }
    }
    TArray<UE::UAI::Contract::FDiagnostic> Issues;
    UeAgentControlRigOps::FControlRigVariableApplyStats Stats;
    UeAgentControlRigOps::ApplyVariablesJson(
        &Blueprint, *Variables, TEXT("provider_extension.variables"), Issues,
        Stats, TEXT("/variables"));
    if (UeAgentControlRigOps::HasBlockingIssue(Issues)) {
      OutError = UeAgentAuthoringDiagnostics::MakeIssuesFailure(
          TEXT("control_rig_variable_apply_failed"), Issues, TEXT("variable_apply"), UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint));
      UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Document, TEXT("variable_apply"), UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint), TEXT("variables"));
      return false;
    }
  }
  // A native variable/hierarchy change may have removed graph nodes. Do not
  // recreate untouched snapshot rows; an explicit edit of a removed node conflicts.
  if (DesiredGraphs) for (const auto& GraphPair : (*DesiredGraphs)->Values) {
    const auto* Context = GraphContexts.Find(GraphPair.Key);
    const auto Graph = GraphPair.Value.IsValid() && GraphPair.Value->Type == EJson::Object ? GraphPair.Value->AsObject() : nullptr;
    const TSharedPtr<FJsonObject>* Nodes = nullptr;
    if (!Context || !Graph || !Graph->TryGetObjectField(TEXT("nodes"), Nodes)) continue;
    for (URigVMGraph* NativeGraph : Blueprint.GetAllModels()) {
      if (!NativeGraph || NativeGraph->GetNodePath() != GraphPair.Key) continue;
      for (const auto& Identity : Context->NodeAliases.SourceIdByAlias) {
        if (NativeGraph->FindNodeByName(FName(*Identity.Value)) || !(*Nodes)->HasField(Identity.Key)) continue;
        if (!Context->UnchangedNodes.Contains(Identity.Key)) {
          OutError = InputFailure(TEXT("control_rig_node_removed_by_native_edit"),
              TEXT("A native hierarchy or variable edit removed this node, but the document also explicitly edits it."),
              *InputDocument, TEXT("/graphs/graphs/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(GraphPair.Key)
                  + TEXT("/nodes/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Identity.Key),
              MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true),
              UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint));
          OutError.SourceKind = TEXT("related_input");
          OutError.Stage = TEXT("graph_apply");
          return false;
        }
        (*Nodes)->RemoveField(Identity.Key);
      }
    }
  }
  const TSharedPtr<FJsonObject> *Graphs = nullptr;
  if (Document->TryGetObjectField(TEXT("graphs"), Graphs) &&
      !ApplyControlRigGraphs(Blueprint, Graphs ? *Graphs : nullptr, OutStats,
                             OutError, GraphContexts)) {
    UeAgentAuthoringDiagnostics::LocateFailure(OutError, *InputDocument,
        TEXT("graph_apply"), UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint));
    return false;
  }
  for (URigVMGraph* NativeGraph : Blueprint.GetAllModels()) {
    const auto* Context = NativeGraph ? GraphContexts.Find(NativeGraph->GetNodePath()) : nullptr;
    if (!Context) continue;
    for (const auto& Removed : Context->RemovedLinks)
      for (const URigVMLink* Link : NativeGraph->GetLinks())
        if (Link && Link->GetSourcePinPath() == Removed.Key && Link->GetTargetPinPath() == Removed.Value) {
          TArray<TSharedPtr<FJsonValue>> Endpoints{MakeShared<FJsonValueString>(Removed.Key), MakeShared<FJsonValueString>(Removed.Value)};
          auto Warning = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("graph_connection_removal_not_retained"), TEXT("connection"),
              MakeShared<FJsonValueNull>(), MakeShared<FJsonValueArray>(MoveTemp(Endpoints)), TEXT("rigging_connection_observation"),
              TEXT("The connection requested for removal is still present after applying the graph."));
          DescribeNativeProjectionFailure(Warning, UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint),
              TEXT("/graphs/graphs/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(NativeGraph->GetNodePath()) + TEXT("/links"));
          UeAgentAuthoringDiagnostics::ReportConsistencyWarning(Warning);
        }
  }
  UeAgentControlRigOps::MarkControlRigModified(&Blueprint);
  return true;
}

TSharedRef<FJsonObject>
FilterGoalProperties(const TSharedPtr<FJsonObject> &Exported) {
  TSharedRef<FJsonObject> Result = CloneObject(Exported);
  const TArray<TSharedPtr<FJsonValue>> *Properties = nullptr;
  TArray<TSharedPtr<FJsonValue>> Filtered;
  if (Result->TryGetArrayField(TEXT("properties"), Properties) && Properties) {
    for (const TSharedPtr<FJsonValue> &Value : *Properties) {
      const TSharedPtr<FJsonObject> Descriptor =
          Value.IsValid() ? Value->AsObject() : nullptr;
      FString Path;
      if (!Descriptor.IsValid() ||
          !Descriptor->TryGetStringField(TEXT("property_path"), Path))
        continue;
      if (Path == TEXT("GoalName") || Path == TEXT("BoneName") ||
          Path == TEXT("CurrentTransform") || Path == TEXT("InitialTransform"))
        continue;
      Filtered.Add(Value);
    }
  }
  Result->SetArrayField(TEXT("properties"), Filtered);
  return Result;
}

TArray<FString> BuildSolverDisplayIds(UIKRigController& Controller) {
  TArray<FString> Names;
  TSet<FString> Reserved;
  for (int32 Index = 0; Index < Controller.GetNumSolvers(); ++Index) {
    const FInstancedStruct* Solver = Controller.GetSolverStructAtIndex(Index);
    const UScriptStruct* Type = Solver ? Solver->GetScriptStruct() : nullptr;
    Names.Add(Type ? Type->GetName() : FString());
    if (Type) Reserved.Add(Type->GetName().ToLower());
  }
  TSet<FString> Used;
  for (FString& Name : Names) {
    if (Name.IsEmpty()) continue;
    const FString Base = Name;
    Name = UE::UAI::AllocateReadableName(Base, [&](const FString& Candidate) {
      return Used.Contains(Candidate.ToLower()) ||
          (!Candidate.Equals(Base, ESearchCase::IgnoreCase) && Reserved.Contains(Candidate.ToLower()));
    });
    Used.Add(Name.ToLower());
  }
  return Names;
}

bool BuildIKRigDocument(UIKRigDefinition &Rig,
                        TSharedPtr<FJsonObject> &OutDocument,
                        UE::UAI::Contract::FDiagnostic& OutError) {
  UIKRigController *Controller = UIKRigController::GetController(&Rig);
  if (!Controller) {
    OutError = NativeDocumentFailure(TEXT("ik_rig_controller_unavailable"),
        TEXT("The IK Rig controller is unavailable."), Rig.GetPathName(), TEXT("/controller_available"),
        MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
    return false;
  }
  TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
  Document->SetStringField(
      TEXT("preview_mesh"),
      Rig.GetPreviewMesh() ? Rig.GetPreviewMesh()->GetPathName() : FString());
  Document->SetStringField(TEXT("retarget_root"),
                           Controller->GetRetargetRoot().ToString());
  TSharedRef<FJsonObject> Goals = MakeShared<FJsonObject>();
  for (UIKRigEffectorGoal *Goal : Controller->GetAllGoals()) {
    if (!Goal)
      continue;
    TSharedRef<FJsonObject> GoalObject = MakeShared<FJsonObject>();
    GoalObject->SetStringField(
        TEXT("bone_name"),
        Controller->GetBoneForGoal(Goal->GoalName).ToString());
    GoalObject->SetStringField(TEXT("transform_space"),
                               TEXT("character_global"));
    const FTransform Current =
        Controller->GetGoalCurrentTransform(Goal->GoalName);
    GoalObject->SetField(
        TEXT("current_transform"),
        EncodeStruct(TBaseStructure<FTransform>::Get(), &Current, Goal));
    UeAgentAuthoringState::FStateSnapshot GoalSnapshot;
    TSharedPtr<FJsonObject> GoalProperties;
    if (!UeAgentDetailsAuthoringService::CaptureObjectProperties(
            Goal, UeAgentDetailsAuthoringService::FCapturePolicy(),
            GoalSnapshot, GoalProperties, OutError)) {
      return false;
    }
    GoalObject->SetObjectField(TEXT("properties"),
                               FilterGoalProperties(GoalProperties));
    Goals->SetObjectField(Goal->GoalName.ToString(), GoalObject);
  }
  UeAgentAuthoringEncodedTypes::FScope::RecordNameKeyedObject(Goals);
  Document->SetObjectField(TEXT("goals"), Goals);

  TSharedRef<FJsonObject> Solvers = MakeShared<FJsonObject>();
  TArray<TSharedPtr<FJsonValue>> SolverOrder;
  const TArray<FString> SolverIds = BuildSolverDisplayIds(*Controller);
  for (int32 Index = 0; Index < Controller->GetNumSolvers(); ++Index) {
    FInstancedStruct *Solver = Controller->GetSolverStructAtIndex(Index);
    const UScriptStruct *Struct = Solver ? Solver->GetScriptStruct() : nullptr;
    if (!Struct || !Solver->GetMemory())
      continue;
    const FString& Id = SolverIds[Index];
    TSharedRef<FJsonObject> SolverObject = MakeShared<FJsonObject>();
    SolverObject->SetStringField(TEXT("struct_path"), Struct->GetPathName());
    SolverObject->SetField(
        TEXT("value"),
        EncodeNativeStorageDeltaFromDefault(const_cast<UScriptStruct *>(Struct),
                                           Solver->GetMemory(), &Rig));
    SolverObject->SetBoolField(TEXT("enabled"),
                               Controller->GetSolverEnabled(Index));
    const FName StartBone = Controller->GetStartBone(Index);
    const FName EndBone = Controller->GetEndBone(Index);
    SolverObject->SetStringField(
        TEXT("start_bone"),
        StartBone.IsNone() ? FString() : StartBone.ToString());
    SolverObject->SetStringField(
        TEXT("end_bone"), EndBone.IsNone() ? FString() : EndBone.ToString());
    Solvers->SetObjectField(Id, SolverObject);
    SolverOrder.Add(MakeShared<FJsonValueString>(Id));
  }
  Document->SetObjectField(TEXT("solvers"), Solvers);
  Document->SetArrayField(TEXT("solver_order"), SolverOrder);

  TSharedRef<FJsonObject> Chains = MakeShared<FJsonObject>();
  for (const FBoneChain &Chain : Controller->GetRetargetChains()) {
    TSharedRef<FJsonObject> ChainObject = MakeShared<FJsonObject>();
    ChainObject->SetStringField(TEXT("start_bone"),
                                Chain.StartBone.BoneName.ToString());
    ChainObject->SetStringField(TEXT("end_bone"),
                                Chain.EndBone.BoneName.ToString());
    const FName GoalName =
        Controller->GetGoal(Chain.IKGoalName) ? Chain.IKGoalName : NAME_None;
    ChainObject->SetStringField(
        TEXT("goal"), GoalName.IsNone() ? FString() : GoalName.ToString());
    Chains->SetObjectField(Chain.ChainName.ToString(), ChainObject);
  }
  UeAgentAuthoringEncodedTypes::FScope::RecordNameKeyedObject(Chains);
  Document->SetObjectField(TEXT("chains"), Chains);
  OutDocument = Document;
  return true;
}

struct FNativeSolverAliases {
  const UScriptStruct* Type = nullptr;
  bool Enabled = true;
  FName StartBone;
  FName EndBone;
  TSet<FName> Goals;
  TMap<FName, FInstancedStruct> GoalSettings;
};

FNativeSolverAliases ReadSolverAliases(UIKRigController& Controller, int32 Index) {
  FNativeSolverAliases Result;
  const FInstancedStruct* Struct = Controller.GetSolverStructAtIndex(Index);
  Result.Type = Struct ? Struct->GetScriptStruct() : nullptr;
  Result.Enabled = Controller.GetSolverEnabled(Index);
  Result.StartBone = Controller.GetStartBone(Index);
  Result.EndBone = Controller.GetEndBone(Index);
  if (auto* Solver = Controller.GetSolverAtIndex(Index)) {
    Solver->GetRequiredGoals(Result.Goals);
    if (Solver->UsesCustomGoalSettings()) {
      const auto* Type = Solver->GetGoalSettingsType();
      for (const FName Goal : Result.Goals)
        if (const auto* Settings = Solver->GetGoalSettings(Goal); Type && Settings)
          Result.GoalSettings.FindOrAdd(Goal).InitializeAs(Type, reinterpret_cast<const uint8*>(Settings));
    }
  }
  return Result;
}

TSharedPtr<FJsonValue> SolverAliasValue(const bool Value) { return MakeShared<FJsonValueBoolean>(Value); }
TSharedPtr<FJsonValue> SolverAliasValue(const FName Value) { return MakeShared<FJsonValueString>(Value.ToString()); }

template<typename T>
bool ResolveSolverAliasIntent(const T& Before, const T& Decoded, const T& Alias,
    const TCHAR* Field, T& Desired, bool& bFromAlias, UE::UAI::Contract::FDiagnostic& OutError) {
  bFromAlias = Alias != Before;
  const bool bValueChanged = Decoded != Before;
  if (bFromAlias && bValueChanged && Alias != Decoded) {
    UeAgentAuthoringDiagnostics::FDiagnostic Diagnostic;
    Diagnostic.Code = TEXT("ik_rig_solver_conflicting_field_edits");
    Diagnostic.Stage = TEXT("document_prepare");
    Diagnostic.PropertyPath = Field;
    Diagnostic.Source.JsonPath = TEXT("/") + FString(Field);
    Diagnostic.PublicMessage = FString::Printf(
        TEXT("value and %s edit the same native solver field to different values."), Field);
    Diagnostic.ActualValue = SolverAliasValue(Alias);
    Diagnostic.ExpectedValue = SolverAliasValue(Decoded);
    Diagnostic.ActualValuePresent = true;
    Diagnostic.ExpectedValuePresent = true;
    Diagnostic.SourceKind = TEXT("authored_value");
    OutError = Diagnostic;
    return false;
  }
  // A complete exported record includes unchanged aliases. They must not undo
  // a real edit in the recursive value, or vice versa.
  Desired = bFromAlias ? Alias : Decoded;
  return true;
}

void LocateSolverFieldFailure(UE::UAI::Contract::FDiagnostic& Error, const FJsonObject& Record,
    const FString& Id, const TCHAR* Field, const FString& TargetPath) {
  const FString Pointer = TEXT("/solvers/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Id);
  if (Error.SourceKind == TEXT("native_result")) {
    if (Error.SemanticPointer.IsEmpty()) Error.SemanticPointer = Pointer + TEXT("/") + Field;
  } else if (Error.SourceKind != TEXT("command_input") && Error.Source.SourceFile.IsEmpty()) {
    Error.Source.JsonPath = TEXT("/") + FString(Field);
    Error.Source.InternalJsonPath = Error.Source.JsonPath;
    UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(Error, Pointer);
  }
  UeAgentAuthoringDiagnostics::LocateFailure(Error, Record,
      TEXT("native_apply"), TargetPath, Field);
}

bool ApplyIKRigDocument(UIKRigDefinition &Rig,
                        const TSharedPtr<const FJsonObject> &Document,
                        FMutationStats &OutStats, UE::UAI::Contract::FDiagnostic& OutError,
                        const FString& TargetPath) {
  if (!ValidateFields(Document,
                      {TEXT("preview_mesh"), TEXT("retarget_root"),
                       TEXT("goals"), TEXT("solvers"), TEXT("solver_order"),
                       TEXT("chains")},
                      TEXT("ik_rig_document"), OutError, TargetPath))
    return false;
  if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(*Document,
      {{TEXT("preview_mesh"), EJson::String, true}, {TEXT("retarget_root"), EJson::String},
       {TEXT("goals"), EJson::Object}, {TEXT("solvers"), EJson::Object},
       {TEXT("solver_order"), EJson::Array}, {TEXT("chains"), EJson::Object}}, OutError)) return false;
  const TSharedPtr<FJsonObject>* SeedGoals = nullptr;
  if (Document->TryGetObjectField(TEXT("goals"), SeedGoals))
    for (const auto& Pair : (*SeedGoals)->Values) {
      if (!Pair.Value || Pair.Value->Type != EJson::Object) {
        OutError = InputFailure(TEXT("ik_rig_goal_invalid"), TEXT("Each IK goal must be an object."), *Document,
            TEXT("/goals/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Pair.Key),
            Pair.Value, MakeShared<FJsonValueString>(TEXT("goal object")), TargetPath);
        return false;
      }
      TSharedPtr<FJsonObject> Properties;
      if (!UeAgentDetailsAuthoringService::ReadPropertyPayload(Pair.Value->AsObject(), TEXT("properties"), Properties, OutError)) return false;
    }
  UIKRigController *Controller = UIKRigController::GetController(&Rig);
  if (!Controller) {
    OutError = NativeDocumentFailure(TEXT("ik_rig_controller_unavailable"),
        TEXT("The IK Rig controller is unavailable."), TargetPath, TEXT("/controller_available"),
        MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
    return false;
  }
  TMap<FString, FNativeSolverAliases> SolverBefore;
  TMap<FName, FName> ChainGoalsBefore;
  for (const auto& Chain : Controller->GetRetargetChains()) ChainGoalsBefore.Add(Chain.ChainName, Chain.IKGoalName);
  TArray<FName> RemovedGoals;
  const TArray<FString> SolverIds = BuildSolverDisplayIds(*Controller);
  for (int32 Index = 0; Index < Controller->GetNumSolvers(); ++Index) {
    const auto Aliases = ReadSolverAliases(*Controller, Index);
    if (Aliases.Type) SolverBefore.Add(SolverIds[Index], Aliases);
  }
  FString PreviewPath;
  if (!Document->HasField(TEXT("preview_mesh"))) {
    OutError = InputFailure(TEXT("ik_rig_preview_mesh_missing"),
        TEXT("The IK Rig document requires preview_mesh; an empty reference is allowed."),
        *Document, TEXT("/preview_mesh"), nullptr,
        MakeShared<FJsonValueString>(TEXT("a skeletal mesh reference or an empty reference")), TargetPath);
    return false;
  }
  if (!UeAgentAuthoringJsonFields::ReadReferenceField(*Document, TEXT("preview_mesh"), PreviewPath, OutError)) return false;
  {
    USkeletalMesh *Preview =
        PreviewPath.IsEmpty()
            ? nullptr
            : UeAgentIKAssetOps::LoadSkeletalMesh(PreviewPath);
    if (!PreviewPath.IsEmpty() && !Preview) {
      OutError = InputFailure(TEXT("ik_rig_preview_mesh_not_found"),
          TEXT("The requested preview mesh could not be loaded as a USkeletalMesh."),
          *Document, TEXT("/preview_mesh"), MakeShared<FJsonValueString>(PreviewPath),
          MakeShared<FJsonValueString>(TEXT("a loadable USkeletalMesh asset")), TargetPath);
      return false;
    }
    if (!Controller->SetSkeletalMesh(Preview)) {
      OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("ik_rig_preview_mesh_apply_failed"),
          TEXT("The IK Rig controller rejected the requested preview skeletal mesh."), TEXT("native_apply"));
      OutError.SourceKind = TEXT("native_result");
      OutError.TargetPath = TargetPath;
      OutError.PropertyPath = TEXT("preview_mesh");
      OutError.ActualValue = MakeShared<FJsonValueString>(PreviewPath);
      OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("a preview mesh accepted by the IK Rig controller"));
      return false;
    }
  }
  FString RetargetRoot;
  if (Document->TryGetStringField(TEXT("retarget_root"), RetargetRoot)) {
    UE::UAI::Contract::FSourceLocation Source;
    if (!UeAgentAuthoringDiagnostics::FScopedSourceLocations::Locate(*Document, TEXT("retarget_root"), Source))
      Source.JsonPath = TEXT("/retarget_root");
    if (!UeAgentIKAssetOps::ApplyRetargetRoot(*Controller, FName(*RetargetRoot), OutError, Source, TargetPath))
      return false;
  }

  const TSharedPtr<FJsonObject> *Goals = nullptr;
  if (Document->TryGetObjectField(TEXT("goals"), Goals) && Goals &&
      Goals->IsValid()) {
    TSet<FName> Desired;
    for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair : (*Goals)->Values)
      Desired.Add(FName(*Pair.Key));
    for (UIKRigEffectorGoal *Goal : Controller->GetAllGoals())
      if (Goal && !Desired.Contains(Goal->GoalName))
        RemovedGoals.Add(Goal->GoalName);
    for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair :
         (*Goals)->Values) {
      const TSharedPtr<FJsonObject> GoalObject =
          Pair.Value.IsValid() ? Pair.Value->AsObject() : nullptr;
      FString BoneName;
      FString Space;
      if (!GoalObject.IsValid() ||
          !GoalObject->TryGetStringField(TEXT("bone_name"), BoneName) ||
          !GoalObject->TryGetStringField(TEXT("transform_space"), Space) ||
          Space != TEXT("character_global")) {
        const bool bBoneString = GoalObject.IsValid() && GoalObject->HasTypedField<EJson::String>(TEXT("bone_name"));
        const FString Field = !GoalObject.IsValid() ? FString() : bBoneString ? TEXT("transform_space") : TEXT("bone_name");
        OutError = InputFailure(TEXT("ik_rig_goal_invalid"),
            Field == TEXT("transform_space") ? TEXT("Goal transform_space must be character_global.") : TEXT("A goal must provide a string bone_name."),
            *Document, TEXT("/goals/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Pair.Key)
                + (Field.IsEmpty() ? FString() : TEXT("/") + Field),
            Field.IsEmpty() ? Pair.Value : GoalObject->TryGetField(Field),
            MakeShared<FJsonValueString>(Field == TEXT("transform_space") ? TEXT("character_global") : TEXT("string")), TargetPath);
        return false;
      }
      const FName GoalName(*Pair.Key);
      if (!Controller->GetGoal(GoalName)) {
        const FName CreatedGoal = Controller->AddNewGoal(GoalName, FName(*BoneName));
        if (CreatedGoal != GoalName) {
          OutError = NativeDocumentFailure(TEXT("ik_rig_goal_create_failed"),
              TEXT("The native controller did not create the requested goal for bone '") + BoneName + TEXT("'."),
              TargetPath, TEXT("/goals/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Pair.Key),
              MakeShared<FJsonValueString>(CreatedGoal.ToString()), MakeShared<FJsonValueString>(GoalName.ToString()));
          return false;
        }
      }
      const FName RequestedBone(*BoneName);
      if (Controller->GetBoneForGoal(GoalName) != RequestedBone &&
          !Controller->SetGoalBone(GoalName, RequestedBone)) {
        OutError = NativeDocumentFailure(TEXT("ik_rig_goal_bone_apply_failed"),
            TEXT("The native controller rejected the requested goal bone."), TargetPath,
            TEXT("/goals/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Pair.Key) + TEXT("/bone_name"),
            MakeShared<FJsonValueString>(Controller->GetBoneForGoal(GoalName).ToString()),
            MakeShared<FJsonValueString>(RequestedBone.ToString()));
        return false;
      }
      FTransform Transform = FTransform::Identity;
      const TSharedPtr<FJsonValue> TransformValue =
          GoalObject->TryGetField(TEXT("current_transform"));
      if (TransformValue.IsValid()) {
        if (!DecodeStruct(TBaseStructure<FTransform>::Get(), TransformValue,
                          &Transform, &Rig, OutError)) {
          LocateDocumentValueFailure(OutError, *Document,
              TEXT("/goals/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Pair.Key) + TEXT("/current_transform"), TargetPath);
          return false;
        }
        Controller->SetGoalCurrentTransform(GoalName, Transform);
      }
      TSharedPtr<FJsonObject> Properties;
      if (!UeAgentDetailsAuthoringService::ReadPropertyPayload(GoalObject, TEXT("properties"), Properties, OutError)) return false;
      if (Properties) {
        UeAgentAuthoringObjectApply::FOptions ApplyOptions;
        ApplyOptions.bAllowDomainAuthorizedOwners = true;
        UeAgentAuthoringDirectApply::FResult ApplyResult;
        if (!UeAgentDetailsAuthoringService::ApplyObjectProperties(Controller->GetGoal(GoalName), Properties, ApplyOptions, ApplyResult)) {
				OutError = ApplyResult.Diagnostic;
          return false;
        }
      }
      ++OutStats.Goals;
    }
  }

  const TSharedPtr<FJsonObject> *Solvers = nullptr;
  const TArray<TSharedPtr<FJsonValue>> *SolverOrder = nullptr;
  if (Document->TryGetObjectField(TEXT("solvers"), Solvers)) {
    if (!Solvers || !Solvers->IsValid() ||
        !Document->TryGetArrayField(TEXT("solver_order"), SolverOrder) ||
        !SolverOrder) {
      const FString Field = !Solvers || !Solvers->IsValid() ? TEXT("solvers") : TEXT("solver_order");
      OutError = InputFailure(TEXT("ik_rig_solver_collection_invalid"),
          TEXT("A solver collection requires a solvers object and a solver_order array."), *Document, TEXT("/") + Field,
          Document->TryGetField(Field), MakeShared<FJsonValueString>(Field == TEXT("solvers") ? TEXT("object") : TEXT("array")), TargetPath);
      return false;
    }
    while (Controller->GetNumSolvers() > 0) {
      const int32 RemovedIndex = Controller->GetNumSolvers() - 1;
      if (!Controller->RemoveSolver(RemovedIndex)) {
        OutError = NativeDocumentFailure(TEXT("ik_rig_solver_remove_failed"),
            FString::Printf(TEXT("The native controller failed to remove solver index %d."), RemovedIndex),
            TargetPath, TEXT("/solvers"), MakeShared<FJsonValueNumber>(Controller->GetNumSolvers()),
            MakeShared<FJsonValueString>(TEXT("all previous solvers removed")));
        return false;
      }
    }
    TSet<FString> Seen;
    for (int32 OrderIndex = 0; OrderIndex < SolverOrder->Num(); ++OrderIndex) {
      const auto& IdValue = (*SolverOrder)[OrderIndex];
      if (!IdValue.IsValid() || IdValue->Type != EJson::String ||
          Seen.Contains(IdValue->AsString())) {
        OutError = InputFailure(TEXT("ik_rig_solver_order_invalid"), TEXT("Each solver_order entry must be a unique string solver ID."),
            *Document, TEXT("/solver_order/") + FString::FromInt(OrderIndex), IdValue,
            MakeShared<FJsonValueString>(TEXT("unique string solver ID")), TargetPath);
        return false;
      }
      const FString Id = IdValue->AsString();
      Seen.Add(Id);
      const TSharedPtr<FJsonObject> *SolverObject = nullptr;
      FString StructPath;
      if (!(*Solvers)->TryGetObjectField(Id, SolverObject) || !SolverObject ||
          !SolverObject->IsValid() ||
          !(*SolverObject)
               ->TryGetStringField(TEXT("struct_path"), StructPath)) {
        const bool bObject = SolverObject && SolverObject->IsValid();
        OutError = InputFailure(TEXT("ik_rig_solver_missing"), TEXT("Each ordered solver must have an object record with a string struct_path."),
            *Document, TEXT("/solvers/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Id) + (bObject ? TEXT("/struct_path") : TEXT("")),
            bObject ? (*SolverObject)->TryGetField(TEXT("struct_path")) : (*Solvers)->TryGetField(Id),
            MakeShared<FJsonValueString>(bObject ? TEXT("script struct path") : TEXT("solver object")), TargetPath);
        return false;
      }
      if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(**SolverObject,
          {{TEXT("enabled"), EJson::Boolean}, {TEXT("start_bone"), EJson::String}, {TEXT("end_bone"), EJson::String}},
          OutError, TEXT("/solvers/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Id))) return false;
      UScriptStruct *Struct = LoadObject<UScriptStruct>(nullptr, *StructPath);
      const int32 Index = Struct ? Controller->AddSolver(Struct) : INDEX_NONE;
      FInstancedStruct *Solver = Index != INDEX_NONE
                                     ? Controller->GetSolverStructAtIndex(Index)
                                     : nullptr;
      auto Before = Solver ? ReadSolverAliases(*Controller, Index) : FNativeSolverAliases();
      if (const auto* Existing = SolverBefore.Find(Id); Existing && Existing->Type == Struct)
        Before = *Existing;
      const TSharedPtr<FJsonValue> SolverValue =
          (*SolverObject)->TryGetField(TEXT("value"));
      if (!Struct || !Solver || !Solver->GetMutableMemory() ||
          !SolverValue.IsValid() ||
          !DecodeStruct(Struct, SolverValue, Solver->GetMutableMemory(), &Rig,
                        OutError)) {
        const FString Pointer = TEXT("/solvers/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Id);
        if (!Struct) {
          OutError = InputFailure(TEXT("ik_rig_solver_value_invalid"), TEXT("The requested solver script struct could not be loaded."),
              *Document, Pointer + TEXT("/struct_path"), MakeShared<FJsonValueString>(StructPath),
              MakeShared<FJsonValueString>(TEXT("loadable solver script struct")), TargetPath);
        } else if (!Solver || !Solver->GetMutableMemory()) {
          OutError = NativeDocumentFailure(TEXT("ik_rig_solver_value_invalid"), TEXT("The native controller did not provide writable storage for the requested solver type."),
              TargetPath, Pointer, MakeShared<FJsonValueString>(StructPath), MakeShared<FJsonValueString>(TEXT("created solver with writable storage")));
        } else if (!SolverValue.IsValid()) {
          OutError = InputFailure(TEXT("ik_rig_solver_value_invalid"), TEXT("A solver record must provide its encoded value."),
              *Document, Pointer + TEXT("/value"), SolverValue, MakeShared<FJsonValueString>(TEXT("encoded solver struct")), TargetPath);
        } else {
          LocateDocumentValueFailure(OutError, *Document, Pointer + TEXT("/value"), TargetPath);
        }
        if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Id;
        return false;
      }
      // Freeze explicit relation intent before the native deletion closure.
      // Existing goal settings may be removed by RemoveGoal; new/edited
      // settings for the same deleted goal are conflicting edits.
      const auto DecodedGoals = ReadSolverAliases(*Controller, Index);
      for (const FName Goal : RemovedGoals) {
        if (!DecodedGoals.Goals.Contains(Goal)) continue;
        const auto* Original = Before.GoalSettings.Find(Goal);
        const auto* Edited = DecodedGoals.GoalSettings.Find(Goal);
        using UE::UAI::StateSemantic::CanonicalJson::FValueComparison;
        FValueComparison Difference;
        if (!Before.Goals.Contains(Goal)) {
          Difference = FValueComparison::Different(TEXT("/required"),
              MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
        } else if (Edited) {
          if (!Original || Edited->GetScriptStruct() != Original->GetScriptStruct()) {
            Difference = FValueComparison::Different(TEXT("/struct_path"),
                Original && Original->GetScriptStruct()
                    ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Original->GetScriptStruct()->GetPathName())) : nullptr,
                Edited->GetScriptStruct()
                    ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Edited->GetScriptStruct()->GetPathName())) : nullptr);
          } else {
            Difference = UeAgentPropertyValueCodec::CompareReflectedStructFields(Edited->GetScriptStruct(),
                Original->GetMemory(), Edited->GetMemory(), &Rig, FCodecOptions());
          }
        }
        if (!Difference.bEquivalent) {
          OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("ik_rig_deleted_goal_has_edited_solver_reference"),
              TEXT("Solver '") + Id + TEXT("' explicitly edits settings for removed goal '") + Goal.ToString() + TEXT("'."),
              TEXT("document_prepare"));
          Difference.ApplyTo(OutError);
          // Settings come from GetGoalSettings, not a proven field of solver.value.
          // Keep their semantic path and mark the solver document as related input.
          OutError.SemanticPointer = TEXT("/goal_settings/")
              + Goal.ToString().Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")) + Difference.Path;
          LocateSolverFieldFailure(OutError, **SolverObject, Id, TEXT("value"), TargetPath);
          OutError.SourceKind = TEXT("related_input");
          return false;
        }
      }
      bool bEnabled;
      if ((*SolverObject)->TryGetBoolField(TEXT("enabled"), bEnabled)) {
        bool Desired, bFromAlias;
        if (!ResolveSolverAliasIntent(Before.Enabled, Controller->GetSolverEnabled(Index),
            bEnabled, TEXT("enabled"), Desired, bFromAlias, OutError)) {
          LocateSolverFieldFailure(OutError, **SolverObject, Id, TEXT("enabled"), TargetPath);
          return false;
        }
        if (Controller->GetSolverEnabled(Index) != Desired &&
            (!Controller->SetSolverEnabled(Index, Desired) ||
             Controller->GetSolverEnabled(Index) != Desired)) {
          OutError = NativeDocumentFailure(TEXT("ik_rig_solver_enabled_apply_failed"),
              TEXT("The native controller rejected or did not retain the requested solver enabled state."), TargetPath,
              TEXT("/solvers/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Id) + TEXT("/enabled"),
              MakeShared<FJsonValueBoolean>(Controller->GetSolverEnabled(Index)), MakeShared<FJsonValueBoolean>(Desired));
          LocateSolverFieldFailure(OutError, **SolverObject, Id,
              bFromAlias ? TEXT("enabled") : TEXT("value"), TargetPath);
          return false;
        }
      }
      FString Bone;
      for (const TCHAR* Field : {TEXT("start_bone"), TEXT("end_bone")}) {
        const bool bEnd = FCString::Strcmp(Field, TEXT("end_bone")) == 0;
        const FName Decoded = bEnd ? Controller->GetEndBone(Index) : Controller->GetStartBone(Index);
        const FName Original = bEnd ? Before.EndBone : Before.StartBone;
        FName Desired = Decoded;
        bool bFromAlias = false;
        if ((*SolverObject)->TryGetStringField(Field, Bone) &&
            !ResolveSolverAliasIntent(Original, Decoded, FName(*Bone), Field,
            Desired, bFromAlias, OutError)) {
          LocateSolverFieldFailure(OutError, **SolverObject, Id, Field, TargetPath);
          return false;
        }
        if (Desired != Original && !UeAgentIKAssetOps::ApplySolverBone(
            *Controller, Index, Desired, bEnd, OutError)) {
          LocateSolverFieldFailure(OutError, **SolverObject, Id,
              bFromAlias ? Field : TEXT("value"), TargetPath);
          return false;
        }
      }
      ++OutStats.Solvers;
    }
    if (Seen.Num() != (*Solvers)->Values.Num()) {
      TArray<FString> Missing;
      for (const auto& Pair : (*Solvers)->Values) if (!Seen.Contains(Pair.Key)) Missing.Add(Pair.Key);
      Missing.Sort();
      OutError = InputFailure(TEXT("ik_rig_solver_order_mismatch"), TEXT("solver_order omits these solver IDs: ") + FString::Join(Missing, TEXT(", ")),
          *Document, TEXT("/solver_order"), Document->TryGetField(TEXT("solver_order")),
          MakeShared<FJsonValueString>(TEXT("every solver ID listed once")), TargetPath);
      return false;
    }
  }

  const TSharedPtr<FJsonObject> *Chains = nullptr;
  if (Document->TryGetObjectField(TEXT("chains"), Chains) && Chains &&
      Chains->IsValid()) {
    TSet<FName> Desired;
    for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair : (*Chains)->Values)
      Desired.Add(FName(*Pair.Key));
    TArray<FName> Remove;
    for (const FBoneChain &Chain : Controller->GetRetargetChains())
      if (!Desired.Contains(Chain.ChainName))
        Remove.Add(Chain.ChainName);
    for (const FName Name : Remove)
      Controller->RemoveRetargetChain(Name);
    for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair :
         (*Chains)->Values) {
      const TSharedPtr<FJsonObject> Chain =
          Pair.Value.IsValid() && Pair.Value->Type == EJson::Object ? Pair.Value->AsObject() : nullptr;
      if (!Chain.IsValid()) {
        OutError = InputFailure(TEXT("ik_rig_chain_invalid"), TEXT("Each retarget chain must be an object."),
            *Document, TEXT("/chains/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Pair.Key),
            Pair.Value, MakeShared<FJsonValueString>(TEXT("chain object")), TargetPath);
        return false;
      }
      if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(*Chain,
          {{TEXT("start_bone"), EJson::String}, {TEXT("end_bone"), EJson::String},
           {TEXT("goal"), EJson::String}}, OutError,
          TEXT("/chains/") + Pair.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")))) return false;
      const FName Name(*Pair.Key);
      const FBoneChain* ExistingChain = Controller->GetRetargetChains().FindByPredicate(
          [Name](const FBoneChain& Candidate) { return Candidate.ChainName == Name; });
      const FBoneChain Before = ExistingChain ? *ExistingChain : FBoneChain();
      FString Start = Before.StartBone.BoneName.ToString();
      FString End = Before.EndBone.BoneName.ToString();
      FString Goal = Before.IKGoalName.ToString();
      Chain->TryGetStringField(TEXT("start_bone"), Start);
      Chain->TryGetStringField(TEXT("end_bone"), End);
      Chain->TryGetStringField(TEXT("goal"), Goal);
      if (RemovedGoals.Contains(FName(*Goal)) &&
          (!ChainGoalsBefore.Contains(Name) || ChainGoalsBefore.FindChecked(Name) != FName(*Goal))) {
        OutError = InputFailure(TEXT("ik_rig_deleted_goal_has_edited_chain_reference"),
            TEXT("The chain explicitly references a goal removed by this document."), *Document,
            TEXT("/chains/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Pair.Key) + TEXT("/goal"),
            MakeShared<FJsonValueString>(Goal), MakeShared<FJsonValueString>(TEXT("a retained goal or an unchanged native relation")), TargetPath);
        OutError.SourceKind = TEXT("related_input");
        return false;
      }
      if (!ExistingChain) {
        const FName CreatedChain = Controller->AddRetargetChain(Name, FName(*Start), FName(*End), FName(*Goal));
        if (CreatedChain != Name) {
          OutError = NativeDocumentFailure(TEXT("ik_rig_chain_create_failed"), TEXT("The native controller did not create the requested retarget chain."),
              TargetPath, TEXT("/chains/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Pair.Key),
              MakeShared<FJsonValueString>(CreatedChain.ToString()), MakeShared<FJsonValueString>(Name.ToString()));
          return false;
        }
      }
      // Native creation permits unset endpoints. Its endpoint setters require a
      // real bone, so replaying an unchanged native default would reject a valid
      // newly created or existing incomplete chain.
      if (ExistingChain) {
        for (const TCHAR* Field : {TEXT("start_bone"), TEXT("end_bone")}) {
          const bool bEnd = FCString::Strcmp(Field, TEXT("end_bone")) == 0;
          const FName Requested(bEnd ? *End : *Start);
          const FName Original = bEnd ? Before.EndBone.BoneName : Before.StartBone.BoneName;
          if (Requested != Original && !(bEnd
              ? Controller->SetRetargetChainEndBone(Name, Requested)
              : Controller->SetRetargetChainStartBone(Name, Requested))) {
            const auto* ActualChain = Controller->GetRetargetChainByName(Name);
            OutError = NativeDocumentFailure(TEXT("ik_rig_chain_apply_failed"), TEXT("The native controller rejected the requested retarget chain endpoint."),
                TargetPath, TEXT("/chains/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Pair.Key) + TEXT("/") + Field,
                ActualChain ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>((bEnd ? ActualChain->EndBone.BoneName : ActualChain->StartBone.BoneName).ToString())) : nullptr,
                MakeShared<FJsonValueString>(Requested.ToString()));
            return false;
          }
        }
      }
      UE::UAI::Contract::FSourceLocation GoalSource;
      if (!UeAgentAuthoringDiagnostics::FScopedSourceLocations::Locate(*Chain, TEXT("goal"), GoalSource))
        GoalSource.JsonPath = TEXT("/chains/") + Pair.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")) + TEXT("/goal");
      if (!UeAgentIKAssetOps::ApplyRetargetChainGoal(*Controller, Name,
          FName(*Goal), OutError, GoalSource, TargetPath)) {
        return false;
      }
      ++OutStats.Chains;
    }
  }
  // Native goal removal owns solver notifications and retarget-chain cleanup.
  // Run it after survivor edits so exported old relationships cannot restore it.
  for (const FName Goal : RemovedGoals)
    if (!Controller->RemoveGoal(Goal)) {
      OutError = NativeDocumentFailure(TEXT("ik_rig_goal_remove_failed"), TEXT("The native controller failed to remove goal '") + Goal.ToString() + TEXT("'."),
          TargetPath, TEXT("/goals/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Goal.ToString()),
          MakeShared<FJsonValueBoolean>(Controller->GetGoal(Goal) != nullptr), MakeShared<FJsonValueBoolean>(false));
      return false;
    }
  Controller->BroadcastNeedsReinitialized();
  Rig.MarkPackageDirty();
  return true;
}

TSharedRef<FJsonObject> BuildRetargetPoses(UIKRetargeterController &Controller,
                                           const ERetargetSourceOrTarget Side,
                                           UIKRetargeter &Owner) {
  TSharedRef<FJsonObject> SideObject = MakeShared<FJsonObject>();
  SideObject->SetStringField(TEXT("root_translation_space"),
                             TEXT("retarget_root_global_translation_delta"));
  SideObject->SetStringField(TEXT("bone_rotation_space"),
                             TEXT("bone_local_rotation_delta"));
  SideObject->SetStringField(
      TEXT("current_pose"),
      Controller.GetCurrentRetargetPoseName(Side).ToString());
  TSharedRef<FJsonObject> Poses = MakeShared<FJsonObject>();
  for (const TPair<FName, FIKRetargetPose> &Pair :
       Controller.GetRetargetPoses(Side)) {
    TSharedRef<FJsonObject> Pose = MakeShared<FJsonObject>();
    const FVector Root = Pair.Value.GetRootTranslationDelta();
    Pose->SetField(TEXT("root_translation"),
                   EncodeStruct(TBaseStructure<FVector>::Get(), &Root, &Owner));
    TSharedRef<FJsonObject> Rotations = MakeShared<FJsonObject>();
    for (const TPair<FName, FQuat> &Rotation :
         Pair.Value.GetAllDeltaRotations()) {
      Rotations->SetField(
          Rotation.Key.ToString(),
          EncodeStruct(TBaseStructure<FQuat>::Get(), &Rotation.Value, &Owner));
    }
    UeAgentAuthoringEncodedTypes::FScope::RecordNameKeyedObject(Rotations);
    Pose->SetObjectField(TEXT("bone_rotations"), Rotations);
    Poses->SetObjectField(Pair.Key.ToString(), Pose);
  }
  UeAgentAuthoringEncodedTypes::FScope::RecordNameKeyedObject(Poses);
  SideObject->SetObjectField(TEXT("poses"), Poses);
  return SideObject;
}

bool BuildIKRetargeterDocument(UIKRetargeter &Retargeter,
                               TSharedPtr<FJsonObject> &OutDocument,
                               UE::UAI::Contract::FDiagnostic& OutError) {
  UIKRetargeterController *Controller =
      UIKRetargeterController::GetController(&Retargeter);
  if (!Controller) {
    OutError = NativeDocumentFailure(TEXT("ik_retargeter_controller_unavailable"),
        TEXT("The IK Retargeter controller is unavailable."), Retargeter.GetPathName(), TEXT("/controller_available"),
        MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
    return false;
  }
  TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
  TSharedRef<FJsonObject> Rigs = MakeShared<FJsonObject>();
  Rigs->SetStringField(
      TEXT("source"),
      Controller->GetIKRig(ERetargetSourceOrTarget::Source)
          ? Controller->GetIKRig(ERetargetSourceOrTarget::Source)->GetPathName()
          : FString());
  Rigs->SetStringField(
      TEXT("target"),
      Controller->GetIKRig(ERetargetSourceOrTarget::Target)
          ? Controller->GetIKRig(ERetargetSourceOrTarget::Target)->GetPathName()
          : FString());
  Document->SetObjectField(TEXT("rigs"), Rigs);
  TSharedRef<FJsonObject> PreviewMeshes = MakeShared<FJsonObject>();
  PreviewMeshes->SetStringField(
      TEXT("source"),
      Controller->GetPreviewMesh(ERetargetSourceOrTarget::Source)
          ? Controller->GetPreviewMesh(ERetargetSourceOrTarget::Source)
                ->GetPathName()
          : FString());
  PreviewMeshes->SetStringField(
      TEXT("target"),
      Controller->GetPreviewMesh(ERetargetSourceOrTarget::Target)
          ? Controller->GetPreviewMesh(ERetargetSourceOrTarget::Target)
                ->GetPathName()
          : FString());
  Document->SetObjectField(TEXT("preview_meshes"), PreviewMeshes);

  TSharedRef<FJsonObject> Ops = MakeShared<FJsonObject>();
  TArray<TSharedPtr<FJsonValue>> OpOrder;
  for (int32 Index = 0; Index < Controller->GetNumRetargetOps(); ++Index) {
    FInstancedStruct *Op = Controller->GetRetargetOpStructAtIndex(Index);
    const UScriptStruct *Struct = Op ? Op->GetScriptStruct() : nullptr;
    if (!Struct || !Op->GetMemory())
      continue;
    const FString Id = Controller->GetOpName(Index).ToString();
    TSharedRef<FJsonObject> OpObject = MakeShared<FJsonObject>();
    OpObject->SetStringField(TEXT("struct_path"), Struct->GetPathName());
    OpObject->SetField(TEXT("value"), EncodeNativeStorageDeltaFromDefault(
                                          const_cast<UScriptStruct *>(Struct),
                                          Op->GetMemory(), &Retargeter,
                                          {TEXT("Name"), TEXT("ParentOpName"),
                                           TEXT("bIsEnabled"), TEXT("ChainMapping")}));
    OpObject->SetBoolField(TEXT("enabled"),
                           Controller->GetRetargetOpEnabled(Index));
    const FName ParentName = Controller->GetParentOpByName(FName(*Id));
    OpObject->SetStringField(TEXT("parent"), ParentName.IsNone()
                                                 ? FString()
                                                 : ParentName.ToString());
    Ops->SetObjectField(Id, OpObject);
    OpOrder.Add(MakeShared<FJsonValueString>(Id));
  }
  UeAgentAuthoringEncodedTypes::FScope::RecordNameKeyedObject(Ops);
  Document->SetObjectField(TEXT("ops"), Ops);
  Document->SetArrayField(TEXT("op_order"), OpOrder);

  TSharedRef<FJsonObject> Mappings = MakeShared<FJsonObject>();
  for (int32 Index = 0; Index < Controller->GetNumRetargetOps(); ++Index) {
    const FName OpName = Controller->GetOpName(Index);
    const FRetargetChainMapping *Mapping = Controller->GetChainMapping(OpName);
    if (!Mapping)
      continue;
    for (const FRetargetChainPair &Pair : Mapping->GetChainPairs()) {
      const FString Id =
          OpName.ToString() + TEXT(":") + Pair.TargetChainName.ToString();
      TSharedRef<FJsonObject> MappingObject = MakeShared<FJsonObject>();
      UeAgentAuthoringEncodedTypes::FScope::SetNameField(MappingObject, TEXT("op"), OpName);
      UeAgentAuthoringEncodedTypes::FScope::SetNameField(MappingObject, TEXT("target_chain"), Pair.TargetChainName);
      UeAgentAuthoringEncodedTypes::FScope::SetNameField(MappingObject, TEXT("source_chain"), Pair.SourceChainName);
      Mappings->SetObjectField(Id, MappingObject);
    }
  }
  UeAgentAuthoringEncodedTypes::FScope::RecordCompositeKeyedObject(Mappings, TEXT(":"), {TEXT("name"), TEXT("name")});
  Document->SetObjectField(TEXT("mappings"), Mappings);
  TSharedRef<FJsonObject> Poses = MakeShared<FJsonObject>();
  Poses->SetObjectField(TEXT("source"),
                        BuildRetargetPoses(*Controller,
                                           ERetargetSourceOrTarget::Source,
                                           Retargeter));
  Poses->SetObjectField(TEXT("target"),
                        BuildRetargetPoses(*Controller,
                                           ERetargetSourceOrTarget::Target,
                                           Retargeter));
  Document->SetObjectField(TEXT("poses"), Poses);
  OutDocument = Document;
  return true;
}

struct FRequestedRetargetPose {
  FVector RootTranslation = FVector::ZeroVector;
  TMap<FName, FQuat> BoneRotations;
  UeAgentIKAssetOps::FRetargetPoseDiagnosticSource DiagnosticSource;
};

UE::UAI::Contract::FSourceLocation RetargetPoseInputSource(
    const FJsonObject& Object, const FString& Field, const FString& Pointer) {
  UE::UAI::Contract::FSourceLocation Source;
  if (!UeAgentAuthoringDiagnostics::FScopedSourceLocations::Locate(Object, Field, Source)) Source.JsonPath = Pointer;
  return Source;
}

UE::UAI::Contract::FDiagnostic RetargetPoseInputFailure(const TCHAR* Code, const FString& Reason,
    const FString& Pointer, const TSharedPtr<FJsonValue>& Actual, const FString& Expected, const FString& TargetPath) {
  auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("document_validation"));
  Diagnostic.SourceKind = TEXT("related_input");
  Diagnostic.Source.JsonPath = Pointer;
  Diagnostic.TargetPath = TargetPath;
  Diagnostic.ActualValuePresent = Actual.IsValid();
  Diagnostic.ActualValue = Actual;
  Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(Expected);
  return Diagnostic;
}

bool ApplyRetargetPoseSide(UIKRetargeterController &Controller,
                           const ERetargetSourceOrTarget Side,
                           const TSharedPtr<FJsonObject> &SideObject,
                           UIKRetargeter &Owner, FMutationStats &OutStats,
                           TMap<FName, FRequestedRetargetPose>& OutRequestedPoses,
                           UE::UAI::Contract::FDiagnostic& OutError, const FString& TargetPath) {
  if (!SideObject.IsValid())
    return true;
  FString RootSpace;
  FString RotationSpace;
  const FString SidePointer = FString(TEXT("/poses/")) + UeAgentIKAssetOps::SourceOrTargetToString(Side);
  if (!SideObject->TryGetStringField(TEXT("root_translation_space"),
                                     RootSpace) ||
      RootSpace != TEXT("retarget_root_global_translation_delta")) {
    OutError = RetargetPoseInputFailure(TEXT("ik_retargeter_pose_space_invalid"),
        TEXT("Root translation must use the retarget root global translation delta space."),
        SidePointer + TEXT("/root_translation_space"), SideObject->TryGetField(TEXT("root_translation_space")),
        TEXT("retarget_root_global_translation_delta"), TargetPath);
    return false;
  }
  if (!SideObject->TryGetStringField(TEXT("bone_rotation_space"),
                                     RotationSpace) ||
      RotationSpace != TEXT("bone_local_rotation_delta")) {
    OutError = RetargetPoseInputFailure(TEXT("ik_retargeter_pose_space_invalid"),
        TEXT("Bone rotations must use the bone local rotation delta space."),
        SidePointer + TEXT("/bone_rotation_space"), SideObject->TryGetField(TEXT("bone_rotation_space")),
        TEXT("bone_local_rotation_delta"), TargetPath);
    return false;
  }
  const TSharedPtr<FJsonObject> *Poses = nullptr;
  if (!SideObject->TryGetObjectField(TEXT("poses"), Poses) || !Poses ||
      !Poses->IsValid()) {
    OutError = RetargetPoseInputFailure(TEXT("ik_retargeter_pose_collection_invalid"),
        TEXT("Retarget poses must be an object keyed by pose name."), SidePointer + TEXT("/poses"),
        SideObject->TryGetField(TEXT("poses")), TEXT("object keyed by pose name"), TargetPath);
    return false;
  }
  TSet<FName> RequestedNames;
  for (const auto& Pair : (*Poses)->Values) {
    const FName Name(*Pair.Key);
    if (RequestedNames.Contains(Name)) {
      OutError = RetargetPoseInputFailure(TEXT("ik_retargeter_pose_identity_duplicate"),
          FString::Printf(TEXT("Pose name '%s' duplicates another pose under native name comparison."), *Pair.Key),
          SidePointer + TEXT("/poses/") + Pair.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")),
          MakeShared<FJsonValueString>(Pair.Key), TEXT("unique native pose name"), TargetPath);
      return false;
    }
    RequestedNames.Add(Name);
  }
  TArray<FName> ExistingNames;
  Controller.GetRetargetPoses(Side).GenerateKeyArray(ExistingNames);
  for (const FName Existing : ExistingNames) {
    if (!RequestedNames.Contains(Existing) &&
        !Controller.RemoveRetargetPose(Existing, Side)) {
      UeAgentAuthoringDiagnostics::FDiagnostic Diagnostic;
      Diagnostic.Code = TEXT("ik_retargeter_pose_remove_rejected");
      Diagnostic.Stage = TEXT("native_apply");
      Diagnostic.PropertyPath = Existing.ToString();
      Diagnostic.Source = RetargetPoseInputSource(*SideObject, TEXT("poses"), SidePointer + TEXT("/poses"));
      Diagnostic.SourceKind = TEXT("related_input");
      Diagnostic.TargetPath = TargetPath;
      Diagnostic.PublicMessage = FString::Printf(TEXT("The native retargeter rejected removal of %s pose '%s'."),
          UeAgentIKAssetOps::SourceOrTargetToString(Side), *Existing.ToString());
      Diagnostic.ExpectedValue = MakeShared<FJsonValueNull>();
      Diagnostic.ActualValue = MakeShared<FJsonValueString>(Existing.ToString());
      OutError = Diagnostic;
      return false;
    }
  }
  for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair : (*Poses)->Values) {
    const FString PosePointer = SidePointer + TEXT("/poses/")
        + Pair.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
    const TSharedPtr<FJsonObject> Pose =
        Pair.Value.IsValid() && Pair.Value->Type == EJson::Object ? Pair.Value->AsObject() : nullptr;
    if (!Pose.IsValid()) {
      OutError = RetargetPoseInputFailure(TEXT("ik_retargeter_pose_expected_object"),
          FString::Printf(TEXT("Retarget pose '%s' must be an object."), *Pair.Key),
          PosePointer, Pair.Value, TEXT("pose object"), TargetPath);
      return false;
    }
    UeAgentIKAssetOps::FRetargetPoseDiagnosticSource DiagnosticSource;
    DiagnosticSource.TargetPath = TargetPath;
    DiagnosticSource.Pose = RetargetPoseInputSource(*Pose, FString(), PosePointer);
    DiagnosticSource.RootTranslation = Pose->HasField(TEXT("root_translation"))
        ? RetargetPoseInputSource(*Pose, TEXT("root_translation"), PosePointer + TEXT("/root_translation")) : DiagnosticSource.Pose;
    DiagnosticSource.Rotations = Pose->HasField(TEXT("bone_rotations"))
        ? RetargetPoseInputSource(*Pose, TEXT("bone_rotations"), PosePointer + TEXT("/bone_rotations")) : DiagnosticSource.Pose;
    const FName PoseName(*Pair.Key);
    if (!Controller.GetRetargetPoses(Side).Contains(PoseName) &&
        Controller.CreateRetargetPose(PoseName, Side).IsNone()) {
      OutError = RetargetPoseInputFailure(TEXT("ik_retargeter_pose_create_failed"),
          FString::Printf(TEXT("The native retargeter rejected creation of pose '%s'."), *Pair.Key),
          PosePointer, MakeShared<FJsonValueString>(Pair.Key), TEXT("creatable pose name"), TargetPath);
      OutError.Stage = TEXT("native_apply");
      return false;
    }
    if (!Controller.SetCurrentRetargetPose(PoseName, Side)) {
      OutError = RetargetPoseInputFailure(TEXT("ik_retargeter_pose_select_failed"),
          FString::Printf(TEXT("The native retargeter could not select pose '%s'."), *Pair.Key),
          PosePointer, MakeShared<FJsonValueString>(Pair.Key), TEXT("selectable pose name"), TargetPath);
      OutError.Stage = TEXT("native_apply");
      return false;
    }
    Controller.ResetRetargetPose(PoseName, {}, Side);
    FVector Root = FVector::ZeroVector;
    if (const TSharedPtr<FJsonValue> Value =
            Pose->TryGetField(TEXT("root_translation"));
        Value.IsValid() && !DecodeStruct(TBaseStructure<FVector>::Get(), Value,
                                         &Root, &Owner, OutError)) {
      UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, PosePointer + TEXT("/root_translation"));
      return false;
    }
    FRequestedRetargetPose& Requested = OutRequestedPoses.Add(PoseName);
    Requested.RootTranslation = Root;
    Requested.DiagnosticSource = DiagnosticSource;
    if (!UeAgentIKAssetOps::ApplyRetargetPoseRootTranslation(Controller, Side, Root, OutError, Requested.DiagnosticSource))
      return false;
    const TSharedPtr<FJsonObject> *Rotations = nullptr;
    if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(*Pose,
        {{TEXT("bone_rotations"), EJson::Object}}, OutError, PosePointer)) return false;
    if (Pose->TryGetObjectField(TEXT("bone_rotations"), Rotations) &&
        Rotations && Rotations->IsValid()) {
      for (const TPair<FString, TSharedPtr<FJsonValue>> &Rotation :
           (*Rotations)->Values) {
        FQuat Quat = FQuat::Identity;
        if (!DecodeStruct(TBaseStructure<FQuat>::Get(), Rotation.Value, &Quat,
                          &Owner, OutError)) {
          UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, PosePointer + TEXT("/bone_rotations/")
              + Rotation.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")));
          return false;
        }
        const FName Bone(*Rotation.Key);
        Requested.BoneRotations.Add(Bone, Quat);
        Requested.DiagnosticSource.BoneRotations.Add(Bone, RetargetPoseInputSource(**Rotations, Rotation.Key,
            PosePointer + TEXT("/bone_rotations/") + Rotation.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"))));
        if (!UeAgentIKAssetOps::ApplyRetargetPoseRotation(Controller, Side, Bone, Quat, OutError, Requested.DiagnosticSource))
          return false;
      }
    }
    ++OutStats.Poses;
  }
  FString Current;
  if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(*SideObject,
      {{TEXT("current_pose"), EJson::String}}, OutError, SidePointer)) return false;
  if (SideObject->TryGetStringField(TEXT("current_pose"), Current) &&
      !Controller.SetCurrentRetargetPose(FName(*Current), Side)) {
    OutError = RetargetPoseInputFailure(TEXT("ik_retargeter_current_pose_invalid"),
        FString::Printf(TEXT("The native retargeter could not select current %s pose '%s'."),
            UeAgentIKAssetOps::SourceOrTargetToString(Side), *Current),
        SidePointer + TEXT("/current_pose"), MakeShared<FJsonValueString>(Current), TEXT("selectable pose name"), TargetPath);
    OutError.Stage = TEXT("native_apply");
    return false;
  }
  return true;
}

bool ApplyIKRetargeterDocument(UIKRetargeter &Retargeter,
                               const TSharedPtr<const FJsonObject> &Document,
                               FMutationStats &OutStats, UE::UAI::Contract::FDiagnostic& OutError,
                               const FString& TargetPath) {
  if (!ValidateFields(Document,
                      {TEXT("rigs"), TEXT("preview_meshes"), TEXT("ops"),
                       TEXT("op_order"), TEXT("mappings"), TEXT("poses")},
                      TEXT("ik_retargeter_document"), OutError, TargetPath))
    return false;
  if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(*Document,
      {{TEXT("rigs"), EJson::Object}, {TEXT("preview_meshes"), EJson::Object}, {TEXT("ops"), EJson::Object},
       {TEXT("op_order"), EJson::Array}, {TEXT("mappings"), EJson::Object}, {TEXT("poses"), EJson::Object}}, OutError)) return false;
  UIKRetargeterController *Controller =
      UIKRetargeterController::GetController(&Retargeter);
  if (!Controller) {
    OutError = NativeDocumentFailure(TEXT("ik_retargeter_controller_unavailable"),
        TEXT("The IK Retargeter controller is unavailable."), TargetPath, TEXT("/controller_available"),
        MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
    return false;
  }
  const TSharedPtr<FJsonObject> *Rigs = nullptr;
  FString SourceRigPath;
  FString TargetRigPath;
  if (!Document->TryGetObjectField(TEXT("rigs"), Rigs) || !Rigs ||
      !Rigs->IsValid() ||
      !(*Rigs)->TryGetStringField(TEXT("source"), SourceRigPath) ||
      !(*Rigs)->TryGetStringField(TEXT("target"), TargetRigPath)) {
    FString Pointer = TEXT("/rigs");
    TSharedPtr<FJsonValue> Actual = Document->TryGetField(TEXT("rigs"));
    FString Expected = TEXT("an object with source and target string references");
    if (Rigs && Rigs->IsValid()) {
      const FString Side = !(*Rigs)->TryGetStringField(TEXT("source"), SourceRigPath) ? TEXT("source") : TEXT("target");
      Pointer += TEXT("/") + Side;
      Actual = (*Rigs)->TryGetField(Side);
      Expected = TEXT("an asset reference string; empty clears the reference");
    }
    OutError = InputFailure(TEXT("ik_retargeter_rig_collection_invalid"),
        TEXT("The retargeter reference collection requires source and target string fields."),
        *Document, Pointer, Actual, MakeShared<FJsonValueString>(Expected), Retargeter.GetPathName());
    return false;
  }
  {
    for (const TPair<ERetargetSourceOrTarget, FString> Side :
         {TPair<ERetargetSourceOrTarget, FString>(
              ERetargetSourceOrTarget::Source, TEXT("source")),
          TPair<ERetargetSourceOrTarget, FString>(
              ERetargetSourceOrTarget::Target, TEXT("target"))}) {
      FString Path;
      if (!(*Rigs)->TryGetStringField(Side.Value, Path))
        continue;
      UIKRigDefinition *Rig =
          Path.IsEmpty() ? nullptr : UeAgentIKAssetOps::LoadIKRig(Path);
      if (!Path.IsEmpty() && !Rig) {
        OutError = InputFailure(TEXT("ik_retargeter_rig_not_found"),
            TEXT("The requested retarget rig could not be loaded as a UIKRigDefinition."),
            *Document, TEXT("/rigs/") + Side.Value, MakeShared<FJsonValueString>(Path),
            MakeShared<FJsonValueString>(TEXT("a loadable UIKRigDefinition asset")), Retargeter.GetPathName());
        return false;
      }
      Controller->SetIKRig(Side.Key, Rig);
    }
  }
  const TSharedPtr<FJsonObject> *PreviewMeshes = nullptr;
  FString SourcePreviewPath;
  FString TargetPreviewPath;
  if (!Document->TryGetObjectField(TEXT("preview_meshes"), PreviewMeshes) ||
      !PreviewMeshes || !PreviewMeshes->IsValid() ||
      !(*PreviewMeshes)
           ->TryGetStringField(TEXT("source"), SourcePreviewPath) ||
      !(*PreviewMeshes)
           ->TryGetStringField(TEXT("target"), TargetPreviewPath)) {
    FString Pointer = TEXT("/preview_meshes");
    TSharedPtr<FJsonValue> Actual = Document->TryGetField(TEXT("preview_meshes"));
    FString Expected = TEXT("an object with source and target string references");
    if (PreviewMeshes && PreviewMeshes->IsValid()) {
      const FString Side = !(*PreviewMeshes)->TryGetStringField(TEXT("source"), SourcePreviewPath) ? TEXT("source") : TEXT("target");
      Pointer += TEXT("/") + Side;
      Actual = (*PreviewMeshes)->TryGetField(Side);
      Expected = TEXT("an asset reference string; empty clears the reference");
    }
    OutError = InputFailure(TEXT("ik_retargeter_preview_mesh_collection_invalid"),
        TEXT("The retargeter reference collection requires source and target string fields."),
        *Document, Pointer, Actual, MakeShared<FJsonValueString>(Expected), Retargeter.GetPathName());
    return false;
  }
  {
    for (const TPair<ERetargetSourceOrTarget, FString> Side :
         {TPair<ERetargetSourceOrTarget, FString>(
              ERetargetSourceOrTarget::Source, TEXT("source")),
          TPair<ERetargetSourceOrTarget, FString>(
              ERetargetSourceOrTarget::Target, TEXT("target"))}) {
      FString Path;
      if (!(*PreviewMeshes)->TryGetStringField(Side.Value, Path))
        continue;
      USkeletalMesh *Mesh =
          Path.IsEmpty() ? nullptr : UeAgentIKAssetOps::LoadSkeletalMesh(Path);
      if (!Path.IsEmpty() && !Mesh) {
        OutError = InputFailure(TEXT("ik_retargeter_preview_mesh_not_found"),
            TEXT("The requested retargeter preview mesh could not be loaded as a USkeletalMesh."),
            *Document, TEXT("/preview_meshes/") + Side.Value, MakeShared<FJsonValueString>(Path),
            MakeShared<FJsonValueString>(TEXT("a loadable USkeletalMesh asset")), Retargeter.GetPathName());
        return false;
      }
      Controller->SetPreviewMesh(Side.Key, Mesh);
    }
  }

  const TSharedPtr<FJsonObject> *Ops = nullptr;
  const TArray<TSharedPtr<FJsonValue>> *OpOrder = nullptr;
  if (Document->TryGetObjectField(TEXT("ops"), Ops)) {
    if (!Ops || !Ops->IsValid() ||
        !Document->TryGetArrayField(TEXT("op_order"), OpOrder) || !OpOrder) {
      const FString Field = !Ops || !Ops->IsValid() ? TEXT("ops") : TEXT("op_order");
      OutError = InputFailure(TEXT("ik_retargeter_op_collection_invalid"),
          TEXT("An op collection requires an ops object and an op_order array."), *Document, TEXT("/") + Field,
          Document->TryGetField(Field), MakeShared<FJsonValueString>(Field == TEXT("ops") ? TEXT("object") : TEXT("array")), TargetPath);
      return false;
    }
    TArray<FString> OrderedIds;
    TSet<FString> Seen;
    for (int32 OrderIndex = 0; OrderIndex < OpOrder->Num(); ++OrderIndex) {
      const auto& IdValue = (*OpOrder)[OrderIndex];
      if (!IdValue.IsValid() || IdValue->Type != EJson::String ||
          Seen.Contains(IdValue->AsString())) {
        OutError = InputFailure(TEXT("ik_retargeter_op_order_invalid"), TEXT("Each op_order entry must be a unique string op ID."),
            *Document, TEXT("/op_order/") + FString::FromInt(OrderIndex), IdValue,
            MakeShared<FJsonValueString>(TEXT("unique string op ID")), TargetPath);
        return false;
      }
      const FString Id = IdValue->AsString();
      Seen.Add(Id);
      const TSharedPtr<FJsonObject> *OpObject = nullptr;
      FString StructPath;
      if (!(*Ops)->TryGetObjectField(Id, OpObject) || !OpObject ||
          !OpObject->IsValid() ||
          !(*OpObject)->TryGetStringField(TEXT("struct_path"), StructPath) ||
          !LoadObject<UScriptStruct>(nullptr, *StructPath)) {
        const bool bObject = OpObject && OpObject->IsValid();
        const bool bPathString = bObject && (*OpObject)->HasTypedField<EJson::String>(TEXT("struct_path"));
        OutError = InputFailure(TEXT("ik_retargeter_op_invalid"),
            bPathString ? TEXT("The requested retarget op script struct could not be loaded.") : TEXT("Each ordered op must have an object record with a string struct_path."),
            *Document, TEXT("/ops/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Id) + (bObject ? TEXT("/struct_path") : TEXT("")),
            bObject ? (*OpObject)->TryGetField(TEXT("struct_path")) : (*Ops)->TryGetField(Id),
            MakeShared<FJsonValueString>(bObject ? TEXT("loadable op script struct") : TEXT("op object")), TargetPath);
        return false;
      }
      FString Parent;
      if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(**OpObject,
          {{TEXT("parent"), EJson::String}, {TEXT("enabled"), EJson::Boolean}}, OutError,
          TEXT("/ops/") + Id.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")))) return false;
      (*OpObject)->TryGetStringField(TEXT("parent"), Parent);
      const FName ParentName(*Parent);
      if (!ParentName.IsNone() && !(*Ops)->HasField(ParentName.ToString())) {
        OutError = InputFailure(TEXT("ik_retargeter_op_parent_missing"), TEXT("The requested parent op is not present in ops."),
            *Document, TEXT("/ops/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Id) + TEXT("/parent"),
            MakeShared<FJsonValueString>(Parent), MakeShared<FJsonValueString>(TEXT("an existing op ID or None")), TargetPath);
        return false;
      }
      OrderedIds.Add(Id);
    }
    if (Seen.Num() != (*Ops)->Values.Num()) {
      TArray<FString> Missing;
      for (const auto& Pair : (*Ops)->Values) if (!Seen.Contains(Pair.Key)) Missing.Add(Pair.Key);
      Missing.Sort();
      OutError = InputFailure(TEXT("ik_retargeter_op_order_mismatch"), TEXT("op_order omits these op IDs: ") + FString::Join(Missing, TEXT(", ")),
          *Document, TEXT("/op_order"), Document->TryGetField(TEXT("op_order")),
          MakeShared<FJsonValueString>(TEXT("every op ID listed once")), TargetPath);
      return false;
    }

    Controller->RemoveAllOps();
    TSet<FString> Created;
    while (Created.Num() < OrderedIds.Num()) {
      const int32 CreatedBefore = Created.Num();
      for (const FString &Id : OrderedIds) {
        if (Created.Contains(Id))
          continue;
        const TSharedPtr<FJsonObject> *OpObject = nullptr;
        (*Ops)->TryGetObjectField(Id, OpObject);
        FString Parent;
        (*OpObject)->TryGetStringField(TEXT("parent"), Parent);
        const FName ParentName(*Parent);
        if (!ParentName.IsNone() && !Created.Contains(ParentName.ToString()))
          continue;

        FString StructPath;
        (*OpObject)->TryGetStringField(TEXT("struct_path"), StructPath);
        UScriptStruct *Struct = LoadObject<UScriptStruct>(nullptr, *StructPath);
        const int32 AddedIndex = Controller->AddRetargetOp(Struct, ParentName);
        FInstancedStruct *Op =
            AddedIndex != INDEX_NONE
                ? Controller->GetRetargetOpStructAtIndex(AddedIndex)
                : nullptr;
        const TSharedPtr<FJsonValue> OpValue =
            (*OpObject)->TryGetField(TEXT("value"));
        if (!Op || !Op->GetMutableMemory() || !OpValue.IsValid() ||
            !DecodeStruct(Struct, OpValue, Op->GetMutableMemory(), &Retargeter,
                          OutError)) {
          const FString Pointer = TEXT("/ops/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Id);
          if (!Op || !Op->GetMutableMemory()) {
            OutError = NativeDocumentFailure(TEXT("ik_retargeter_op_value_invalid"), TEXT("The native controller did not provide writable storage for the requested op type."),
                TargetPath, Pointer, MakeShared<FJsonValueString>(StructPath), MakeShared<FJsonValueString>(TEXT("created op with writable storage")));
          } else if (!OpValue.IsValid()) {
            OutError = InputFailure(TEXT("ik_retargeter_op_value_invalid"), TEXT("An op record must provide its encoded value."),
                *Document, Pointer + TEXT("/value"), OpValue, MakeShared<FJsonValueString>(TEXT("encoded op struct")), TargetPath);
          } else {
            LocateDocumentValueFailure(OutError, *Document, Pointer + TEXT("/value"), TargetPath);
          }
          if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Id;
          return false;
        }
        const FName ResolvedName =
            Controller->SetOpName(FName(*Id), AddedIndex);
        if (ResolvedName.ToString() != Id) {
          OutError = NativeDocumentFailure(TEXT("ik_retargeter_op_name_not_unique"), TEXT("The native controller resolved the requested op ID to a different name."),
              TargetPath, TEXT("/ops/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Id),
              MakeShared<FJsonValueString>(ResolvedName.ToString()), MakeShared<FJsonValueString>(Id));
          return false;
        }
        const int32 ResolvedIndex =
            Controller->GetIndexOfOpByName(ResolvedName);
        bool bEnabled = true;
        (*OpObject)->TryGetBoolField(TEXT("enabled"), bEnabled);
        Controller->SetRetargetOpEnabled(ResolvedIndex, bEnabled);
        Created.Add(Id);
        ++OutStats.RetargetOps;
      }
      if (Created.Num() == CreatedBefore) {
        OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("ik_retargeter_op_parent_cycle"),
            TEXT("The remaining ops cannot be created because their parent dependencies do not resolve."), TEXT("document_prepare"));
        OutError.TargetPath = TargetPath;
        for (const FString& PendingId : OrderedIds) {
          if (Created.Contains(PendingId)) continue;
          const auto Pending = (*Ops)->GetObjectField(PendingId);
          OutError.Append(InputFailure(TEXT("ik_retargeter_op_parent_cycle"), TEXT("This parent dependency is still unresolved."),
              *Document, TEXT("/ops/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(PendingId) + TEXT("/parent"),
              Pending->TryGetField(TEXT("parent")), MakeShared<FJsonValueString>(TEXT("a parent that can be created without a cycle")), TargetPath));
        }
        return false;
      }
    }
    for (int32 TargetIndex = 0; TargetIndex < OrderedIds.Num(); ++TargetIndex) {
      const int32 CurrentIndex =
          Controller->GetIndexOfOpByName(FName(*OrderedIds[TargetIndex]));
      if (CurrentIndex != TargetIndex &&
          !Controller->MoveRetargetOpInStack(CurrentIndex, TargetIndex)) {
        OutError = NativeDocumentFailure(TEXT("ik_retargeter_op_order_apply_failed"),
            TEXT("The native controller failed to move op '") + OrderedIds[TargetIndex] + TEXT("' to its requested stack position."),
            TargetPath, TEXT("/op_order/") + FString::FromInt(TargetIndex),
            MakeShared<FJsonValueNumber>(Controller->GetIndexOfOpByName(FName(*OrderedIds[TargetIndex]))),
            MakeShared<FJsonValueNumber>(TargetIndex));
        return false;
      }
    }
    for (const FString &Id : OrderedIds) {
      const TSharedPtr<FJsonObject> *OpObject = nullptr;
      (*Ops)->TryGetObjectField(Id, OpObject);
      FString Parent;
      (*OpObject)->TryGetStringField(TEXT("parent"), Parent);
      const FName ExpectedParent(*Parent);
      if (Controller->GetParentOpByName(FName(*Id)) != ExpectedParent) {
        OutError = NativeDocumentFailure(TEXT("ik_retargeter_op_parent_verify_failed"), TEXT("The native op parent does not match the requested parent."),
            TargetPath, TEXT("/ops/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Id) + TEXT("/parent"),
            MakeShared<FJsonValueString>(Controller->GetParentOpByName(FName(*Id)).ToString()),
            MakeShared<FJsonValueString>(ExpectedParent.ToString()));
        return false;
      }
    }
  }

  const TSharedPtr<FJsonObject> *Mappings = nullptr;
  const auto MappingDiagnosticSource = [&TargetPath](const FJsonObject& Mapping, const FString& Key) {
    UeAgentIKAssetOps::FRetargetMappingDiagnosticSource Source;
    Source.TargetPath = TargetPath;
    const FString Base = TEXT("/mappings/") + Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
    if (!UeAgentAuthoringDiagnostics::FScopedSourceLocations::Locate(Mapping, FString(), Source.Mapping))
      Source.Mapping.JsonPath = Base;
    if (!UeAgentAuthoringDiagnostics::FScopedSourceLocations::Locate(Mapping, TEXT("source_chain"), Source.SourceChain))
      Source.SourceChain.JsonPath = Base + TEXT("/source_chain");
    if (!UeAgentAuthoringDiagnostics::FScopedSourceLocations::Locate(Mapping, TEXT("target_chain"), Source.TargetChain))
      Source.TargetChain.JsonPath = Base + TEXT("/target_chain");
    return Source;
  };
  if (Document->TryGetObjectField(TEXT("mappings"), Mappings) && Mappings &&
      Mappings->IsValid()) {
    Controller->AutoMapChains(EAutoMapChainType::Clear, true);
    for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair :
         (*Mappings)->Values) {
      const TSharedPtr<FJsonObject> Mapping =
          Pair.Value.IsValid() && Pair.Value->Type == EJson::Object ? Pair.Value->AsObject() : nullptr;
      FString Op;
      FString Target;
      FString Source;
      if (!Mapping.IsValid() || !Mapping->TryGetStringField(TEXT("op"), Op) ||
          !Mapping->TryGetStringField(TEXT("target_chain"), Target) ||
          !Mapping->TryGetStringField(TEXT("source_chain"), Source) ||
          Pair.Key != Op + TEXT(":") + Target) {
        FString Field;
        if (Mapping.IsValid()) for (const TCHAR* Candidate : {TEXT("op"), TEXT("target_chain"), TEXT("source_chain")})
          if (!Mapping->HasTypedField<EJson::String>(Candidate)) { Field = Candidate; break; }
        const bool bKeyMismatch = Mapping.IsValid() && Field.IsEmpty();
        OutError = InputFailure(TEXT("ik_retargeter_mapping_invalid"),
            bKeyMismatch ? TEXT("The mapping key must equal op + ':' + target_chain.") : TEXT("A mapping must be an object with string op, target_chain and source_chain fields."),
            *Document, TEXT("/mappings/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Pair.Key) + (Field.IsEmpty() ? FString() : TEXT("/") + Field),
            bKeyMismatch ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Pair.Key)) : (Field.IsEmpty() ? Pair.Value : Mapping->TryGetField(Field)),
            MakeShared<FJsonValueString>(bKeyMismatch ? Op + TEXT(":") + Target : (Field.IsEmpty() ? TEXT("mapping object") : TEXT("string"))), TargetPath);
        return false;
      }
      if (!UeAgentIKAssetOps::ApplyRetargetChainMapping(*Controller,
          FName(*Source), FName(*Target), FName(*Op), OutError, MappingDiagnosticSource(*Mapping, Pair.Key))) return false;
      ++OutStats.Mappings;
    }
  }
  const TSharedPtr<FJsonObject> *Poses = nullptr;
  TMap<FName, FRequestedRetargetPose> RequestedSourcePoses;
  TMap<FName, FRequestedRetargetPose> RequestedTargetPoses;
  if (Document->TryGetObjectField(TEXT("poses"), Poses) && Poses &&
      Poses->IsValid()) {
    if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(**Poses,
        {{TEXT("source"), EJson::Object}, {TEXT("target"), EJson::Object}}, OutError, TEXT("/poses"))) return false;
    const TSharedPtr<FJsonObject> *Source = nullptr;
    const TSharedPtr<FJsonObject> *Target = nullptr;
    if ((*Poses)->TryGetObjectField(TEXT("source"), Source) &&
        !ApplyRetargetPoseSide(*Controller, ERetargetSourceOrTarget::Source,
                               Source ? *Source : nullptr, Retargeter, OutStats,
                               RequestedSourcePoses, OutError, TargetPath))
      return false;
    if ((*Poses)->TryGetObjectField(TEXT("target"), Target) &&
        !ApplyRetargetPoseSide(*Controller, ERetargetSourceOrTarget::Target,
                               Target ? *Target : nullptr, Retargeter, OutStats,
                               RequestedTargetPoses, OutError, TargetPath))
      return false;
  }
  Controller->CleanAsset();
  // Keep the original decoded request across native cleanup. A capture made
  // after cleanup would adopt any dropped mappings or rotations as expected.
  if (Mappings && Mappings->IsValid()) {
    for (const auto& Pair : (*Mappings)->Values) {
      const auto Mapping = Pair.Value->AsObject();
      if (!UeAgentIKAssetOps::VerifyRetargetChainMapping(*Controller,
          FName(*Mapping->GetStringField(TEXT("source_chain"))),
          FName(*Mapping->GetStringField(TEXT("target_chain"))),
          FName(*Mapping->GetStringField(TEXT("op"))), OutError, MappingDiagnosticSource(*Mapping, Pair.Key))) return false;
    }
  }
  for (const auto& Pair : RequestedSourcePoses)
    if (!UeAgentIKAssetOps::VerifyRetargetPose(*Controller, ERetargetSourceOrTarget::Source,
        Pair.Key, Pair.Value.RootTranslation, Pair.Value.BoneRotations, OutError, Pair.Value.DiagnosticSource)) return false;
  for (const auto& Pair : RequestedTargetPoses)
    if (!UeAgentIKAssetOps::VerifyRetargetPose(*Controller, ERetargetSourceOrTarget::Target,
        Pair.Key, Pair.Value.RootTranslation, Pair.Value.BoneRotations, OutError, Pair.Value.DiagnosticSource)) return false;
  Retargeter.MarkPackageDirty();
  return true;
}
} // namespace

FString NormalizePackagePath(const FString &InPath) {
  FString Path = InPath.TrimStartAndEnd();
  if (Path.Contains(TEXT(".")))
    Path = FPackageName::ObjectPathToPackageName(Path);
  return Path;
}

FString ToObjectPath(const FString &PackagePath) {
  const FString Name = FPackageName::GetLongPackageAssetName(PackagePath);
  return Name.IsEmpty() ? PackagePath : PackagePath + TEXT(".") + Name;
}

UObject *LoadAsset(const EAssetKind Kind, const FString &InPath) {
  switch (Kind) {
  case EAssetKind::ControlRig:
    return UeAgentControlRigOps::LoadControlRigBlueprint(InPath);
  case EAssetKind::IKRig:
    return UeAgentIKAssetOps::LoadIKRig(InPath);
  case EAssetKind::IKRetargeter:
    return UeAgentIKAssetOps::LoadIKRetargeter(InPath);
  default:
    return nullptr;
  }
}

UObject *CreateFactorySeed(const EAssetKind Kind, UObject *Outer,
                           const FName Name, const EObjectFlags Flags,
                           UE::UAI::Contract::FDiagnostic& OutError) {
  OutError.Reset();
  if (!Outer) {
    OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("rigging_factory_outer_missing"),
        TEXT("A Rigging asset seed cannot be created without a destination outer object."), TEXT("authoring_prepare"));
    OutError.SourceKind = TEXT("native_result");
    OutError.PropertyPath = TEXT("outer_available");
    OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
    OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
    return nullptr;
  }
  UFactory *Factory = nullptr;
  UClass *AssetClass = nullptr;
  switch (Kind) {
  case EAssetKind::ControlRig:
    Factory = NewObject<UControlRigBlueprintFactory>();
    AssetClass = UControlRigBlueprint::StaticClass();
    break;
  case EAssetKind::IKRig:
    Factory = NewObject<UIKRigDefinitionFactory>();
    AssetClass = UIKRigDefinition::StaticClass();
    break;
  case EAssetKind::IKRetargeter:
    Factory = NewObject<UIKRetargetFactory>();
    AssetClass = UIKRetargeter::StaticClass();
    break;
  default:
    break;
  }
  UObject *Result = Factory && AssetClass
                        ? Factory->FactoryCreateNew(AssetClass, Outer, Name,
                                                    Flags, nullptr, GWarn)
                        : nullptr;
  if (!Result) {
    OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("rigging_factory_create_failed"),
        !AssetClass ? TEXT("The requested Rigging asset kind has no factory class mapping.")
        : !Factory ? TEXT("Unreal could not allocate the factory for this Rigging asset kind.")
        : TEXT("The Rigging factory returned no asset seed."), TEXT("authoring_prepare"));
    OutError.SourceKind = TEXT("native_result");
    OutError.PropertyPath = !AssetClass ? TEXT("asset_kind") : !Factory ? TEXT("factory_available") : TEXT("asset_created");
    if (!AssetClass) {
      OutError.ActualValue = MakeShared<FJsonValueString>(AssetKindToString(Kind));
      TArray<TSharedPtr<FJsonValue>> Allowed;
      for (const auto Supported : {EAssetKind::ControlRig, EAssetKind::IKRig, EAssetKind::IKRetargeter})
        Allowed.Add(MakeShared<FJsonValueString>(AssetKindToString(Supported)));
      OutError.ExpectedValue = MakeShared<FJsonValueArray>(MoveTemp(Allowed));
    } else {
      OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
      OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
    }
  }
  return Result;
}

bool BuildDocument(const EAssetKind Kind, UObject &Asset,
                   TSharedPtr<FJsonObject> &OutDocument, UE::UAI::Contract::FDiagnostic& OutError,
                   const FString& CanonicalAssetPath) {
  OutDocument.Reset();
  const FString TargetPath = CanonicalAssetPath.IsEmpty() ? FString() : ToObjectPath(CanonicalAssetPath);
  const auto FinishCapture = [&](const bool bSuccess) {
    if (!bSuccess) BindNativeCaptureTarget(OutError, Asset, TargetPath);
    return bSuccess;
  };
  switch (Kind) {
  case EAssetKind::ControlRig:
    if (UControlRigBlueprint *Blueprint = Cast<UControlRigBlueprint>(&Asset))
      return FinishCapture(BuildControlRigDocument(*Blueprint, OutDocument, OutError));
    break;
  case EAssetKind::IKRig:
    if (UIKRigDefinition *Rig = Cast<UIKRigDefinition>(&Asset))
      return FinishCapture(BuildIKRigDocument(*Rig, OutDocument, OutError));
    break;
  case EAssetKind::IKRetargeter:
    if (UIKRetargeter *Retargeter = Cast<UIKRetargeter>(&Asset))
      return FinishCapture(BuildIKRetargeterDocument(*Retargeter, OutDocument, OutError));
    break;
  default:
    break;
  }
  OutError = AssetTypeFailure(TEXT("rigging_asset_type_mismatch"), TEXT("state_capture"), Kind, Asset);
  return FinishCapture(false);
}

bool ApplyDocument(const EAssetKind Kind, UObject &Asset,
                   const TSharedPtr<const FJsonObject> &Document,
                   FMutationStats &OutStats, UE::UAI::Contract::FDiagnostic& OutError,
                   const TSharedPtr<const FJsonObject>& CapturedDocument, const FString& CanonicalAssetPath) {
  OutStats = FMutationStats();
  OutError.Reset();
  if (!Document.IsValid())
    return true;
  const FString TargetPath = CanonicalAssetPath.IsEmpty() ? Asset.GetPathName() : ToObjectPath(CanonicalAssetPath);
  const auto FinishApply = [&](const bool bSuccess) {
    if (!bSuccess) UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Document, TEXT("document_apply"), TargetPath);
    return bSuccess;
  };
  switch (Kind) {
  case EAssetKind::ControlRig:
    if (UControlRigBlueprint *Blueprint = Cast<UControlRigBlueprint>(&Asset))
      return FinishApply(ApplyControlRigDocument(*Blueprint, Document, OutStats, OutError, CapturedDocument));
    break;
  case EAssetKind::IKRig:
    if (UIKRigDefinition *Rig = Cast<UIKRigDefinition>(&Asset))
      return FinishApply(ApplyIKRigDocument(*Rig, Document, OutStats, OutError, TargetPath));
    break;
  case EAssetKind::IKRetargeter:
    if (UIKRetargeter *Retargeter = Cast<UIKRetargeter>(&Asset))
      return FinishApply(ApplyIKRetargeterDocument(*Retargeter, Document, OutStats, OutError, TargetPath));
    OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("rigging_asset_type_mismatch"),
        TEXT("Applying an IK Retargeter document requires an IK Retargeter asset."), TEXT("document_apply"));
    OutError.SourceKind = TEXT("native_result");
    OutError.TargetPath = CanonicalAssetPath.IsEmpty() ? Asset.GetPathName() : ToObjectPath(CanonicalAssetPath);
    OutError.ActualValue = MakeShared<FJsonValueString>(Asset.GetClass()->GetPathName());
    OutError.ExpectedValue = MakeShared<FJsonValueString>(UIKRetargeter::StaticClass()->GetPathName());
    return false;
  default:
    break;
  }
  OutError = AssetTypeFailure(TEXT("rigging_asset_type_mismatch"), TEXT("document_apply"), Kind, Asset);
  if (!CanonicalAssetPath.IsEmpty()) OutError.TargetPath = ToObjectPath(CanonicalAssetPath);
  return false;
}

bool CompileAsset(const EAssetKind Kind, UObject &Asset, UE::UAI::Contract::FDiagnostic& OutError) {
  OutError.Reset();
  if (Kind == EAssetKind::ControlRig) {
    UControlRigBlueprint *Blueprint = Cast<UControlRigBlueprint>(&Asset);
    if (!Blueprint) {
      OutError = AssetTypeFailure(TEXT("control_rig_compile_target_invalid"), TEXT("compile"), EAssetKind::ControlRig, Asset);
      return false;
    }
    FCompilerResultsLog CompilerLog;
    FKismetEditorUtilities::CompileBlueprint(
        Blueprint, EBlueprintCompileOptions::None, &CompilerLog);
    if (CompilerLog.NumErrors > 0 || Blueprint->Status == BS_Error) {
      OutError = UeAgentAuthoringDiagnostics::MakeCompileFailure(
          TEXT("control_rig_compile_failed"), Blueprint->GetPathName(), CompilerLog, Blueprint);
      return false;
    }
    return true;
  }
  if (Kind == EAssetKind::IKRig) {
    UIKRigDefinition *Rig = Cast<UIKRigDefinition>(&Asset);
    if (UIKRigController *Controller =
            Rig ? UIKRigController::GetController(Rig) : nullptr) {
      Controller->BroadcastNeedsReinitialized();
      return true;
    }
    if (!Rig) OutError = AssetTypeFailure(TEXT("ik_rig_compile_target_invalid"), TEXT("compile"), EAssetKind::IKRig, Asset);
    else {
      OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("ik_rig_compile_target_invalid"),
          TEXT("The IK Rig asset has no controller available for reinitialization."), TEXT("compile"));
      OutError.SourceKind = TEXT("native_result");
      OutError.TargetPath = Asset.GetPathName();
      OutError.PropertyPath = TEXT("controller_available");
      OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
      OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
    }
    return false;
  }
  UIKRetargeter *Retargeter = Cast<UIKRetargeter>(&Asset);
  if (UIKRetargeterController *Controller =
          Retargeter ? UIKRetargeterController::GetController(Retargeter)
                     : nullptr) {
    Controller->CleanAsset();
    return true;
  }
  if (!Retargeter) OutError = AssetTypeFailure(TEXT("ik_retargeter_compile_target_invalid"), TEXT("compile"), EAssetKind::IKRetargeter, Asset);
  else {
    OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("ik_retargeter_compile_target_invalid"),
        TEXT("The IK Retargeter asset has no controller available for cleanup."), TEXT("compile"));
    OutError.SourceKind = TEXT("native_result");
    OutError.TargetPath = Asset.GetPathName();
    OutError.PropertyPath = TEXT("controller_available");
    OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
    OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
  }
  return false;
}

bool SaveAsset(const EAssetKind Kind, UObject &Asset, UE::UAI::Contract::FDiagnostic& OutError) {
  return Kind == EAssetKind::ControlRig
             ? UeAgentControlRigOps::SaveAssetPackage(&Asset, OutError)
             : UeAgentIKAssetOps::SaveAssetPackage(&Asset, OutError);
}
} // namespace UeAgentRiggingAuthoring
