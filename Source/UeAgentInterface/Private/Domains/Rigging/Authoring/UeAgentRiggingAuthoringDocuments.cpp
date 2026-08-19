// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Rigging/Authoring/UeAgentRiggingAuthoringDocuments.h"

#include "ControlRigBlueprint.h"
#include "ControlRigBlueprintFactory.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Domains/Rigging/UeAgentControlRigDomainBackend.h"
#include "Domains/Rigging/UeAgentIKDomainBackend.h"
#include "Engine/SkeletalMesh.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "RetargetEditor/IKRetargetFactory.h"
#include "RetargetEditor/IKRetargeterController.h"
#include "Retargeter/IKRetargetChainMapping.h"
#include "Retargeter/IKRetargeter.h"
#include "Rig/IKRigDefinition.h"
#include "RigEditor/IKRigController.h"
#include "RigEditor/IKRigDefinitionFactory.h"
#include "RigVMModel/Nodes/RigVMTemplateNode.h"
#include "RigVMModel/Nodes/RigVMUnitNode.h"
#include "RigVMModel/Nodes/RigVMVariableNode.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/RigVMLink.h"
#include "RigVMModel/RigVMNode.h"
#include "RigVMModel/RigVMPin.h"
#include "Rigs/RigHierarchy.h"
#include "Rigs/RigHierarchyController.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/StructOnScope.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"
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
EncodeStructDeltaFromNativeDefault(UScriptStruct *Struct, const void *Value,
                                   UObject *Owner = nullptr) {
  TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
  TSharedPtr<FJsonObject> Fields = MakeShared<FJsonObject>();
  if (Struct && Value) {
    FStructOnScope NativeDefault(Struct);
    const void *DefaultMemory = NativeDefault.GetStructMemory();
    FString CaptureError;
    const FString RootOwnerPath =
        Owner ? Owner->GetPathName()
              : FString::Printf(TEXT("struct:%s"), *Struct->GetPathName());
    UeAgentDetailsAuthoringService::CaptureStructDeltaFields(
        Struct, Value, DefaultMemory, RootOwnerPath, Owner, Fields,
        CaptureError);
  }
  Result->SetObjectField(TEXT("fields"), Fields);
  return MakeShared<FJsonValueObject>(Result);
}

bool DecodeStruct(UScriptStruct *Struct, const TSharedPtr<FJsonValue> &Value,
                  void *OutValue, UObject *Owner, FString &OutError) {
  FCodecOptions Options;
  Options.bAllowPartialStructFields = true;
  return UeAgentPropertyValueCodec::DecodeStructValue(Struct, Value, OutValue,
                                                      Owner, Options, OutError);
}

TSharedPtr<FJsonValue>
EncodeControlSettings(const FRigControlSettings &Settings, UObject *Owner) {
  TSharedPtr<FJsonValue> Encoded = EncodeStructDeltaFromNativeDefault(
      FRigControlSettings::StaticStruct(), &Settings, Owner);
  bool bAnyLimitEnabled = false;
  for (const FRigControlLimitEnabled &Limit : Settings.LimitEnabled) {
    if (Limit.bMinimum || Limit.bMaximum) {
      bAnyLimitEnabled = true;
      break;
    }
  }
  if (!bAnyLimitEnabled) {
    const TSharedPtr<FJsonObject> Object =
        Encoded.IsValid() ? Encoded->AsObject() : nullptr;
    const TSharedPtr<FJsonObject> *Fields = nullptr;
    if (Object.IsValid() && Object->TryGetObjectField(TEXT("fields"), Fields) &&
        Fields && Fields->IsValid()) {
      (*Fields)->RemoveField(TEXT("LimitEnabled"));
      (*Fields)->RemoveField(TEXT("MinimumValue"));
      (*Fields)->RemoveField(TEXT("MaximumValue"));
    }
  }
  return Encoded;
}

TSharedRef<FJsonObject> CloneObject(const TSharedPtr<FJsonObject> &Source) {
  return Source.IsValid()
             ? UeAgentDetailsAuthoring::CloneObjectTree(Source).ToSharedRef()
             : MakeShared<FJsonObject>();
}

TSharedRef<FJsonObject> MakeLiteral(const bool Value) {
  TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
  Result->SetBoolField(TEXT("literal"), Value);
  return Result;
}

TSharedRef<FJsonObject> MakeLiteral(const double Value) {
  TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
  Result->SetNumberField(TEXT("literal"), Value);
  return Result;
}

bool ReadLiteral(const TSharedPtr<FJsonValue> &Value, bool &OutValue,
                 FString &OutError) {
  const TSharedPtr<FJsonObject> Object =
      Value.IsValid() ? Value->AsObject() : nullptr;
  if (!Object.IsValid() || Object->Values.Num() != 1 ||
      !Object->TryGetBoolField(TEXT("literal"), OutValue)) {
    OutError = TEXT("expected_boolean_literal");
    return false;
  }
  return true;
}

bool ReadLiteral(const TSharedPtr<FJsonValue> &Value, double &OutValue,
                 FString &OutError) {
  const TSharedPtr<FJsonObject> Object =
      Value.IsValid() ? Value->AsObject() : nullptr;
  if (!Object.IsValid() || Object->Values.Num() != 1 ||
      !Object->TryGetNumberField(TEXT("literal"), OutValue) ||
      !FMath::IsFinite(OutValue)) {
    OutError = TEXT("expected_numeric_literal");
    return false;
  }
  return true;
}

bool ValidateFields(const TSharedPtr<const FJsonObject> &Object,
                    const TSet<FString> &Allowed, const FString &Prefix,
                    FString &OutError) {
  if (!Object.IsValid())
    return true;
  for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair : Object->Values) {
    if (!Allowed.Contains(Pair.Key)) {
      OutError = Prefix + TEXT("_unknown_field:") + Pair.Key;
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
        MakeLiteral(static_cast<double>(Value.Get<int32>())));
  case ERigControlType::Vector2D: {
    const FVector3f Stored = Value.Get<FVector3f>();
    const FVector2D Canonical(Stored.X, Stored.Y);
    return EncodeStruct(TBaseStructure<FVector2D>::Get(), &Canonical);
  }
  case ERigControlType::Position:
  case ERigControlType::Scale: {
    const FVector Canonical(Value.Get<FVector3f>());
    return EncodeStruct(TBaseStructure<FVector>::Get(), &Canonical);
  }
  case ERigControlType::Rotator: {
    const FRotator Canonical =
        FRotator::MakeFromEuler(FVector(Value.Get<FVector3f>()));
    return EncodeStruct(TBaseStructure<FRotator>::Get(), &Canonical);
  }
  case ERigControlType::Transform:
  case ERigControlType::TransformNoScale:
  case ERigControlType::EulerTransform: {
    const FTransform Canonical =
        Value.GetAsTransform(Settings.ControlType, Settings.PrimaryAxis);
    return EncodeStruct(TBaseStructure<FTransform>::Get(), &Canonical);
  }
  default:
    return MakeShared<FJsonValueNull>();
  }
}

bool DecodeControlValue(const TSharedPtr<FJsonValue> &Value,
                        const FRigControlSettings &Settings,
                        FRigControlValue &OutValue, FString &OutError) {
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
    double Decoded = 0.0;
    if (!ReadLiteral(Value, Decoded, OutError))
      return false;
    OutValue.Set<float>(static_cast<float>(Decoded));
    return true;
  }
  case ERigControlType::Integer: {
    double Decoded = 0.0;
    if (!ReadLiteral(Value, Decoded, OutError) ||
        FMath::FloorToDouble(Decoded) != Decoded || Decoded < MIN_int32 ||
        Decoded > MAX_int32) {
      OutError = TEXT("invalid_control_integer");
      return false;
    }
    OutValue.Set<int32>(static_cast<int32>(Decoded));
    return true;
  }
  case ERigControlType::Vector2D: {
    FVector2D Decoded = FVector2D::ZeroVector;
    if (!DecodeStruct(TBaseStructure<FVector2D>::Get(), Value, &Decoded,
                      nullptr, OutError))
      return false;
    OutValue.Set<FVector3f>(FVector3f(Decoded.X, Decoded.Y, 0.0f));
    return true;
  }
  case ERigControlType::Position:
  case ERigControlType::Scale: {
    FVector Decoded = FVector::ZeroVector;
    if (!DecodeStruct(TBaseStructure<FVector>::Get(), Value, &Decoded, nullptr,
                      OutError))
      return false;
    OutValue.Set<FVector3f>(FVector3f(Decoded));
    return true;
  }
  case ERigControlType::Rotator: {
    FRotator Decoded = FRotator::ZeroRotator;
    if (!DecodeStruct(TBaseStructure<FRotator>::Get(), Value, &Decoded, nullptr,
                      OutError))
      return false;
    OutValue.Set<FVector3f>(FVector3f(Decoded.Euler()));
    return true;
  }
  case ERigControlType::Transform:
  case ERigControlType::TransformNoScale:
  case ERigControlType::EulerTransform: {
    FTransform Decoded = FTransform::Identity;
    if (!DecodeStruct(TBaseStructure<FTransform>::Get(), Value, &Decoded,
                      nullptr, OutError))
      return false;
    OutValue.SetFromTransform(Decoded, Settings.ControlType,
                              Settings.PrimaryAxis);
    return true;
  }
  default:
    OutError = TEXT("unsupported_control_value_type");
    return false;
  }
}

TSharedRef<FJsonObject> BuildRigElement(URigHierarchy &Hierarchy,
                                        const FRigElementKey &Key) {
  TSharedRef<FJsonObject> Element = MakeShared<FJsonObject>();
  Element->SetStringField(TEXT("kind"), ElementKind(Key.Type));
  Element->SetStringField(TEXT("name"), Key.Name.ToString());
  const FRigElementKey Parent = Hierarchy.GetFirstParent(Key);
  if (Parent.IsValid()) {
    TSharedRef<FJsonObject> ParentObject = MakeShared<FJsonObject>();
    ParentObject->SetStringField(TEXT("kind"), ElementKind(Parent.Type));
    ParentObject->SetStringField(TEXT("name"), Parent.Name.ToString());
    Element->SetObjectField(TEXT("parent"), ParentObject);
  } else {
    Element->SetField(TEXT("parent"), MakeShared<FJsonValueNull>());
  }
  return Element;
}

bool BuildControlRigDocument(UControlRigBlueprint &Blueprint,
                             TSharedPtr<FJsonObject> &OutDocument,
                             FString &OutError) {
  OutError.Reset();
  TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
  Document->SetStringField(TEXT("rig_type"), Blueprint.ControlRigType ==
                                                     EControlRigType::RigModule
                                                 ? TEXT("rig_module")
                                             : Blueprint.ControlRigType ==
                                                     EControlRigType::ModularRig
                                                 ? TEXT("modular_rig")
                                                 : TEXT("control_rig"));
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
          Key.Type != ERigElementType::Curve)
        continue;
      TSharedRef<FJsonObject> Element = BuildRigElement(*Hierarchy, Key);
      if (Key.Type == ERigElementType::Bone ||
          Key.Type == ERigElementType::Null) {
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
        Element->SetField(TEXT("settings"),
                          EncodeControlSettings(Control->Settings, &Blueprint));
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
      Elements->SetObjectField(ElementId(Key), Element);
    }
  }
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
    for (URigVMNode *Node : Graph->GetNodes()) {
      if (!Node)
        continue;
      TSharedRef<FJsonObject> NodeObject = MakeShared<FJsonObject>();
      const FVector2D Position = Node->GetPosition();
      NodeObject->SetField(TEXT("position"),
                           EncodeStruct(TBaseStructure<FVector2D>::Get(),
                                        &Position, &Blueprint));
      if (const URigVMUnitNode *Unit = Cast<URigVMUnitNode>(Node)) {
        NodeObject->SetStringField(TEXT("node_type"), TEXT("unit"));
        NodeObject->SetStringField(TEXT("script_struct"),
                                   Unit->GetScriptStruct()
                                       ? Unit->GetScriptStruct()->GetPathName()
                                       : FString());
        NodeObject->SetStringField(TEXT("method"),
                                   Unit->GetMethodName().ToString());
      } else if (const URigVMVariableNode *Variable =
                     Cast<URigVMVariableNode>(Node)) {
        NodeObject->SetStringField(TEXT("node_type"), TEXT("variable"));
        NodeObject->SetStringField(TEXT("variable_name"),
                                   Variable->GetVariableName().ToString());
        NodeObject->SetStringField(TEXT("cpp_type"), Variable->GetCPPType());
        NodeObject->SetStringField(TEXT("default_value"),
                                   Variable->GetDefaultValue());
      } else if (const URigVMTemplateNode *Template =
                     Cast<URigVMTemplateNode>(Node)) {
        NodeObject->SetStringField(TEXT("node_type"), TEXT("template"));
        NodeObject->SetStringField(TEXT("notation"),
                                   Template->GetNotation().ToString());
      } else {
        NodeObject->SetStringField(TEXT("node_type"),
                                   Node->GetClass()->GetPathName());
      }
      TSharedRef<FJsonObject> PinDefaults = MakeShared<FJsonObject>();
      for (URigVMPin *Pin : Node->GetPins()) {
        if (!Pin || Pin->GetDirection() == ERigVMPinDirection::Output ||
            Pin->GetDirection() == ERigVMPinDirection::Hidden)
          continue;
        const FString DefaultValue = Pin->GetDefaultValue();
        if (!DefaultValue.IsEmpty() && DefaultValue != TEXT("()"))
          PinDefaults->SetStringField(Pin->GetName(), DefaultValue);
      }
      NodeObject->SetObjectField(TEXT("pin_defaults"), PinDefaults);
      Nodes->SetObjectField(Node->GetName(), NodeObject);
    }
    GraphObject->SetObjectField(TEXT("nodes"), Nodes);
    TArray<TSharedPtr<FJsonValue>> Links;
    for (URigVMLink *Link : Graph->GetLinks()) {
      if (!Link)
        continue;
      TArray<TSharedPtr<FJsonValue>> Pair;
      Pair.Add(MakeShared<FJsonValueString>(Link->GetSourcePinPath()));
      Pair.Add(MakeShared<FJsonValueString>(Link->GetTargetPinPath()));
      Links.Add(MakeShared<FJsonValueArray>(Pair));
    }
    GraphObject->SetArrayField(TEXT("links"), Links);
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
    FMutationStats &OutStats, FString &OutError) {
  if (!HierarchyObject.IsValid())
    return true;
  if (!ValidateFields(HierarchyObject,
                      {TEXT("transform_space"), TEXT("elements")},
                      TEXT("control_rig_hierarchy"), OutError))
    return false;
  FString Space;
  if (HierarchyObject->TryGetStringField(TEXT("transform_space"), Space) &&
      Space != TEXT("parent_local_initial")) {
    OutError = TEXT("control_rig_hierarchy_transform_space_invalid");
    return false;
  }
  const TSharedPtr<FJsonObject> *Elements = nullptr;
  if (!HierarchyObject->TryGetObjectField(TEXT("elements"), Elements))
    return true;
  if (!Elements || !Elements->IsValid()) {
    OutError = TEXT("control_rig_hierarchy_elements_invalid");
    return false;
  }
  URigHierarchy *Hierarchy = Blueprint.GetHierarchy();
  URigHierarchyController *Controller =
      Hierarchy ? Hierarchy->GetController(true) : nullptr;
  if (!Hierarchy || !Controller) {
    OutError = TEXT("control_rig_hierarchy_controller_unavailable");
    return false;
  }

  TSet<FRigElementKey> Desired;
  for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair :
       (*Elements)->Values) {
    const TSharedPtr<FJsonObject> Element =
        Pair.Value.IsValid() ? Pair.Value->AsObject() : nullptr;
    FString Kind;
    FString Name;
    ERigElementType Type = ERigElementType::None;
    if (!Element.IsValid() || !Element->TryGetStringField(TEXT("kind"), Kind) ||
        !Element->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty() ||
        !ParseElementKind(Kind, Type)) {
      OutError = TEXT("control_rig_element_identity_invalid:") + Pair.Key;
      return false;
    }
    const FRigElementKey Key(FName(*Name), Type);
    if (Pair.Key != ElementId(Key) || Desired.Contains(Key)) {
      OutError =
          TEXT("control_rig_element_key_invalid_or_duplicate:") + Pair.Key;
      return false;
    }
    Desired.Add(Key);
  }

  TArray<FRigElementKey> ExistingKeys = Hierarchy->GetAllKeys(false);
  Algo::Reverse(ExistingKeys);
  for (const FRigElementKey &Key : ExistingKeys) {
    if (Desired.Contains(Key))
      continue;
    bool bCanRemove = Key.Type == ERigElementType::Control ||
                      Key.Type == ERigElementType::Null ||
                      Key.Type == ERigElementType::Curve;
    if (const FRigBoneElement *Bone = Hierarchy->Find<FRigBoneElement>(Key))
      bCanRemove = Bone->BoneType == ERigBoneType::User;
    if (bCanRemove)
      Controller->RemoveElement(Key, false, false);
  }

  TSet<FString> Applied;
  while (Applied.Num() < (*Elements)->Values.Num()) {
    const int32 Before = Applied.Num();
    for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair :
         (*Elements)->Values) {
      if (Applied.Contains(Pair.Key))
        continue;
      const TSharedPtr<FJsonObject> Element = Pair.Value->AsObject();
      FString Kind;
      FString Name;
      Element->TryGetStringField(TEXT("kind"), Kind);
      Element->TryGetStringField(TEXT("name"), Name);
      ERigElementType Type = ERigElementType::None;
      ParseElementKind(Kind, Type);
      const FRigElementKey Key(FName(*Name), Type);
      FRigElementKey Parent;
      const TSharedPtr<FJsonValue> ParentValue =
          Element->TryGetField(TEXT("parent"));
      if (ParentValue.IsValid() && ParentValue->Type != EJson::Null) {
        const TSharedPtr<FJsonObject> ParentObject = ParentValue->AsObject();
        FString ParentKind;
        FString ParentName;
        ERigElementType ParentType = ERigElementType::None;
        if (!ParentObject.IsValid() ||
            !ParentObject->TryGetStringField(TEXT("kind"), ParentKind) ||
            !ParentObject->TryGetStringField(TEXT("name"), ParentName) ||
            !ParseElementKind(ParentKind, ParentType)) {
          OutError = TEXT("control_rig_parent_identity_invalid:") + Pair.Key;
          return false;
        }
        Parent = FRigElementKey(FName(*ParentName), ParentType);
        if (Hierarchy->GetIndex(Parent) == INDEX_NONE)
          continue;
      }

      FTransform InitialLocal = FTransform::Identity;
      if (Type == ERigElementType::Bone || Type == ERigElementType::Null) {
        const TSharedPtr<FJsonValue> TransformValue =
            Element->TryGetField(TEXT("initial_local_transform"));
        if (TransformValue.IsValid() &&
            !DecodeStruct(TBaseStructure<FTransform>::Get(), TransformValue,
                          &InitialLocal, &Blueprint, OutError)) {
          OutError = Pair.Key + TEXT(":initial_local_transform:") + OutError;
          return false;
        }
      }

      if (Hierarchy->GetIndex(Key) == INDEX_NONE) {
        if (Type == ERigElementType::Bone) {
          ERigBoneType BoneType = ERigBoneType::User;
          FString BoneTypeText;
          if (Element->TryGetStringField(TEXT("bone_type"), BoneTypeText)) {
            const int64 Value =
                StaticEnum<ERigBoneType>()->GetValueByNameString(BoneTypeText);
            if (Value != INDEX_NONE)
              BoneType = static_cast<ERigBoneType>(Value);
          }
          Controller->AddBone(Key.Name, Parent, InitialLocal, false, BoneType,
                              false, false);
        } else if (Type == ERigElementType::Null) {
          Controller->AddNull(Key.Name, Parent, InitialLocal, false, false,
                              false);
        } else if (Type == ERigElementType::Curve) {
          double CurveValue = 0.0;
          const TSharedPtr<FJsonValue> Value =
              Element->TryGetField(TEXT("value"));
          if (Value.IsValid() && !ReadLiteral(Value, CurveValue, OutError))
            return false;
          Controller->AddCurve(Key.Name, static_cast<float>(CurveValue), false,
                               false);
          Hierarchy->SetCurveValue(Key, static_cast<float>(CurveValue), false);
        } else if (Type == ERigElementType::Control) {
          FRigControlSettings Settings;
          const TSharedPtr<FJsonValue> SettingsValue =
              Element->TryGetField(TEXT("settings"));
          if (!SettingsValue.IsValid() ||
              !DecodeStruct(FRigControlSettings::StaticStruct(), SettingsValue,
                            &Settings, &Blueprint, OutError)) {
            OutError = Pair.Key + TEXT(":settings:") + OutError;
            return false;
          }
          if (Settings.LimitEnabled.IsEmpty()) {
            Settings.SetupLimitArrayForType();
          }
          FRigControlValue ControlValue;
          const TSharedPtr<FJsonValue> InitialValue =
              Element->TryGetField(TEXT("initial_value"));
          if (!InitialValue.IsValid() ||
              !DecodeControlValue(InitialValue, Settings, ControlValue,
                                  OutError)) {
            OutError = Pair.Key + TEXT(":initial_value:") + OutError;
            return false;
          }
          FTransform Offset = FTransform::Identity;
          FTransform Shape = FTransform::Identity;
          if (const TSharedPtr<FJsonValue> Value =
                  Element->TryGetField(TEXT("initial_offset_transform"));
              Value.IsValid() &&
              !DecodeStruct(TBaseStructure<FTransform>::Get(), Value, &Offset,
                            &Blueprint, OutError))
            return false;
          if (const TSharedPtr<FJsonValue> Value =
                  Element->TryGetField(TEXT("initial_shape_transform"));
              Value.IsValid() &&
              !DecodeStruct(TBaseStructure<FTransform>::Get(), Value, &Shape,
                            &Blueprint, OutError))
            return false;
          Controller->AddControl(Key.Name, Parent, Settings, ControlValue,
                                 Offset, Shape, false, false);
        }
      } else {
        if (Parent.IsValid())
          Controller->SetParent(Key, Parent, false, false, false);
        else
          Controller->RemoveAllParents(Key, false, false, false);
        if (Type == ERigElementType::Bone || Type == ERigElementType::Null) {
          Hierarchy->SetLocalTransform(Key, InitialLocal, true, true, false,
                                       false);
          Hierarchy->SetLocalTransform(Key, InitialLocal, false, true, false,
                                       false);
        } else if (Type == ERigElementType::Curve) {
          double CurveValue = 0.0;
          if (const TSharedPtr<FJsonValue> Value =
                  Element->TryGetField(TEXT("value"));
              Value.IsValid() && !ReadLiteral(Value, CurveValue, OutError))
            return false;
          Hierarchy->SetCurveValue(Key, static_cast<float>(CurveValue), false);
        } else if (Type == ERigElementType::Control) {
          FRigControlSettings Settings;
          if (!DecodeStruct(FRigControlSettings::StaticStruct(),
                            Element->TryGetField(TEXT("settings")), &Settings,
                            &Blueprint, OutError))
            return false;
          if (Settings.LimitEnabled.IsEmpty()) {
            Settings.SetupLimitArrayForType();
          }
          Controller->SetControlSettings(Key, Settings, false);
          FRigControlValue ControlValue;
          if (!DecodeControlValue(Element->TryGetField(TEXT("initial_value")),
                                  Settings, ControlValue, OutError))
            return false;
          Hierarchy->SetControlValue(
              Key, ControlValue, ERigControlValueType::Initial, false, false);
          Hierarchy->SetControlValue(
              Key, ControlValue, ERigControlValueType::Current, false, false);
          FTransform Offset = FTransform::Identity;
          FTransform Shape = FTransform::Identity;
          if (!DecodeStruct(
                  TBaseStructure<FTransform>::Get(),
                  Element->TryGetField(TEXT("initial_offset_transform")),
                  &Offset, &Blueprint, OutError) ||
              !DecodeStruct(
                  TBaseStructure<FTransform>::Get(),
                  Element->TryGetField(TEXT("initial_shape_transform")), &Shape,
                  &Blueprint, OutError))
            return false;
          Hierarchy->SetControlOffsetTransform(Key, Offset, true, false, false,
                                               false);
          Hierarchy->SetControlOffsetTransform(Key, Offset, false, false, false,
                                               false);
          Hierarchy->SetControlShapeTransform(Key, Shape, true, false);
          Hierarchy->SetControlShapeTransform(Key, Shape, false, false);
        }
      }
      Applied.Add(Pair.Key);
      ++OutStats.HierarchyElements;
    }
    if (Applied.Num() == Before) {
      OutError = TEXT("control_rig_hierarchy_parent_cycle_or_missing_parent");
      return false;
    }
  }
  return true;
}

bool ApplyControlRigGraphs(UControlRigBlueprint &Blueprint,
                           const TSharedPtr<const FJsonObject> &GraphsObject,
                           FMutationStats &OutStats, FString &OutError) {
  if (!GraphsObject.IsValid())
    return true;
  if (!ValidateFields(GraphsObject, {TEXT("position_space"), TEXT("graphs")},
                      TEXT("control_rig_graphs"), OutError))
    return false;
  FString Space;
  if (GraphsObject->TryGetStringField(TEXT("position_space"), Space) &&
      Space != TEXT("rigvm_graph_canvas_2d")) {
    OutError = TEXT("control_rig_graph_position_space_invalid");
    return false;
  }
  const TSharedPtr<FJsonObject> *Graphs = nullptr;
  if (!GraphsObject->TryGetObjectField(TEXT("graphs"), Graphs))
    return true;
  if (!Graphs || !Graphs->IsValid()) {
    OutError = TEXT("control_rig_graphs_invalid");
    return false;
  }
  TArray<TSharedPtr<FJsonValue>> LegacyGraphs;
  for (const TPair<FString, TSharedPtr<FJsonValue>> &GraphPair :
       (*Graphs)->Values) {
    const TSharedPtr<FJsonObject> Graph =
        GraphPair.Value.IsValid() ? GraphPair.Value->AsObject() : nullptr;
    if (!Graph.IsValid()) {
      OutError = TEXT("control_rig_graph_invalid:") + GraphPair.Key;
      return false;
    }
    TSharedRef<FJsonObject> LegacyGraph = MakeShared<FJsonObject>();
    LegacyGraph->SetStringField(TEXT("name"), GraphPair.Key);
    TArray<TSharedPtr<FJsonValue>> LegacyNodes;
    const TSharedPtr<FJsonObject> *Nodes = nullptr;
    if (Graph->TryGetObjectField(TEXT("nodes"), Nodes) && Nodes &&
        Nodes->IsValid()) {
      for (const TPair<FString, TSharedPtr<FJsonValue>> &NodePair :
           (*Nodes)->Values) {
        const TSharedPtr<FJsonObject> Node =
            NodePair.Value.IsValid() ? NodePair.Value->AsObject() : nullptr;
        if (!Node.IsValid())
          return false;
        TSharedRef<FJsonObject> LegacyNode = CloneObject(Node);
        LegacyNode->SetStringField(TEXT("name"), NodePair.Key);
        const TSharedPtr<FJsonValue> PositionValue =
            Node->TryGetField(TEXT("position"));
        if (PositionValue.IsValid()) {
          FVector2D Position = FVector2D::ZeroVector;
          if (!DecodeStruct(TBaseStructure<FVector2D>::Get(), PositionValue,
                            &Position, &Blueprint, OutError))
            return false;
          TSharedRef<FJsonObject> LegacyPosition = MakeShared<FJsonObject>();
          LegacyPosition->SetNumberField(TEXT("x"), Position.X);
          LegacyPosition->SetNumberField(TEXT("y"), Position.Y);
          LegacyNode->SetObjectField(TEXT("position"), LegacyPosition);
        }
        LegacyNodes.Add(MakeShared<FJsonValueObject>(LegacyNode));
        ++OutStats.GraphNodes;
      }
    }
    LegacyGraph->SetArrayField(TEXT("nodes"), LegacyNodes);
    TArray<TSharedPtr<FJsonValue>> LegacyLinks;
    const TArray<TSharedPtr<FJsonValue>> *Links = nullptr;
    if (Graph->TryGetArrayField(TEXT("links"), Links) && Links) {
      for (const TSharedPtr<FJsonValue> &LinkValue : *Links) {
        const TArray<TSharedPtr<FJsonValue>> *LinkPair = nullptr;
        if (!LinkValue.IsValid() || !LinkValue->TryGetArray(LinkPair) ||
            !LinkPair || LinkPair->Num() != 2 || !(*LinkPair)[0].IsValid() ||
            (*LinkPair)[0]->Type != EJson::String ||
            !(*LinkPair)[1].IsValid() ||
            (*LinkPair)[1]->Type != EJson::String) {
          OutError = TEXT("control_rig_graph_link_invalid:") + GraphPair.Key;
          return false;
        }
        TSharedRef<FJsonObject> LegacyLink = MakeShared<FJsonObject>();
        LegacyLink->SetStringField(TEXT("source"), (*LinkPair)[0]->AsString());
        LegacyLink->SetStringField(TEXT("target"), (*LinkPair)[1]->AsString());
        LegacyLinks.Add(MakeShared<FJsonValueObject>(LegacyLink));
        ++OutStats.GraphLinks;
      }
    }
    LegacyGraph->SetArrayField(TEXT("links"), LegacyLinks);
    LegacyGraphs.Add(MakeShared<FJsonValueObject>(LegacyGraph));
  }
  TSharedRef<FJsonObject> LegacyRoot = MakeShared<FJsonObject>();
  // The structured workspace is an exact graph snapshot.  Rebuild the graph so
  // nodes and links omitted from HJSON are removed instead of silently surviving
  // a merge-only apply.
  LegacyRoot->SetBoolField(TEXT("replace_nodes"), true);
  LegacyRoot->SetArrayField(TEXT("graphs"), LegacyGraphs);
  TArray<TSharedPtr<FJsonValue>> Issues;
  if (!UeAgentControlRigOps::ApplyGraphsJson(
          &Blueprint, LegacyRoot, TEXT("provider_extension.graphs"), Issues)) {
    OutError = TEXT("control_rig_graph_apply_failed");
    if (!Issues.IsEmpty() && Issues[0].IsValid() &&
        Issues[0]->AsObject().IsValid())
      Issues[0]->AsObject()->TryGetStringField(TEXT("message"), OutError);
    return false;
  }
  return true;
}

bool ApplyControlRigDocument(UControlRigBlueprint &Blueprint,
                             const TSharedPtr<const FJsonObject> &Document,
                             FMutationStats &OutStats, FString &OutError) {
  if (!ValidateFields(Document,
                      {TEXT("rig_type"), TEXT("preview_mesh"),
                       TEXT("shape_libraries"), TEXT("hierarchy"),
                       TEXT("graphs"), TEXT("variables")},
                      TEXT("control_rig_document"), OutError))
    return false;
  FString PreviewMesh;
  if (Document->TryGetStringField(TEXT("preview_mesh"), PreviewMesh)) {
    TSharedRef<FJsonObject> Preview = MakeShared<FJsonObject>();
    Preview->SetStringField(TEXT("preview_skeletal_mesh"), PreviewMesh);
    TArray<TSharedPtr<FJsonValue>> Issues;
    if (!UeAgentControlRigOps::ApplyPreviewJson(
            &Blueprint, Preview, TEXT("provider_extension.preview_mesh"),
            Issues)) {
      OutError = TEXT("control_rig_preview_apply_failed");
      return false;
    }
  }
  const TArray<TSharedPtr<FJsonValue>> *ShapeLibraries = nullptr;
  if (Document->TryGetArrayField(TEXT("shape_libraries"), ShapeLibraries) &&
      ShapeLibraries) {
    TSharedRef<FJsonObject> ShapeReferences = MakeShared<FJsonObject>();
    ShapeReferences->SetArrayField(TEXT("shape_libraries"), *ShapeLibraries);
    TArray<TSharedPtr<FJsonValue>> Issues;
    if (!UeAgentControlRigOps::ApplyShapeLibrariesJson(
            &Blueprint, ShapeReferences,
            TEXT("provider_extension.shape_libraries"), Issues)) {
      OutError = TEXT("control_rig_shape_library_apply_failed");
      return false;
    }
  }
  const TSharedPtr<FJsonObject> *Hierarchy = nullptr;
  if (Document->TryGetObjectField(TEXT("hierarchy"), Hierarchy) &&
      !ApplyControlRigHierarchy(Blueprint, Hierarchy ? *Hierarchy : nullptr,
                                OutStats, OutError))
    return false;
  const TSharedPtr<FJsonObject> *Variables = nullptr;
  if (Document->TryGetObjectField(TEXT("variables"), Variables) && Variables &&
      Variables->IsValid()) {
    TArray<TSharedPtr<FJsonValue>> Issues;
    UeAgentControlRigOps::FControlRigVariableApplyStats Stats;
    UeAgentControlRigOps::ApplyVariablesJson(
        &Blueprint, *Variables, TEXT("provider_extension.variables"), Issues,
        Stats);
    if (UeAgentControlRigOps::HasBlockingIssue(Issues)) {
      OutError = TEXT("control_rig_variable_apply_failed");
      for (const TSharedPtr<FJsonValue> &IssueValue : Issues) {
        const TSharedPtr<FJsonObject> Issue =
            IssueValue.IsValid() ? IssueValue->AsObject() : nullptr;
        if (!Issue.IsValid())
          continue;
        FString Code;
        FString Message;
        Issue->TryGetStringField(TEXT("code"), Code);
        Issue->TryGetStringField(TEXT("message"), Message);
        if (!Code.IsEmpty() || !Message.IsEmpty()) {
          OutError += TEXT(":") + Code + TEXT(":") + Message;
          break;
        }
      }
      return false;
    }
  }
  const TSharedPtr<FJsonObject> *Graphs = nullptr;
  if (Document->TryGetObjectField(TEXT("graphs"), Graphs) &&
      !ApplyControlRigGraphs(Blueprint, Graphs ? *Graphs : nullptr, OutStats,
                             OutError))
    return false;
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

bool BuildIKRigDocument(UIKRigDefinition &Rig,
                        TSharedPtr<FJsonObject> &OutDocument,
                        FString &OutError) {
  UIKRigController *Controller = UIKRigController::GetController(&Rig);
  if (!Controller) {
    OutError = TEXT("ik_rig_controller_unavailable");
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
  Document->SetObjectField(TEXT("goals"), Goals);

  TSharedRef<FJsonObject> Solvers = MakeShared<FJsonObject>();
  TArray<TSharedPtr<FJsonValue>> SolverOrder;
  TMap<FString, int32> TypeOccurrences;
  for (int32 Index = 0; Index < Controller->GetNumSolvers(); ++Index) {
    FInstancedStruct *Solver = Controller->GetSolverStructAtIndex(Index);
    const UScriptStruct *Struct = Solver ? Solver->GetScriptStruct() : nullptr;
    if (!Struct || !Solver->GetMemory())
      continue;
    const int32 Occurrence = TypeOccurrences.FindOrAdd(Struct->GetPathName())++;
    const FString Id =
        Struct->GetName() + TEXT("#") + FString::FromInt(Occurrence);
    TSharedRef<FJsonObject> SolverObject = MakeShared<FJsonObject>();
    SolverObject->SetStringField(TEXT("struct_path"), Struct->GetPathName());
    SolverObject->SetField(
        TEXT("value"),
        EncodeStructDeltaFromNativeDefault(const_cast<UScriptStruct *>(Struct),
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
  Document->SetObjectField(TEXT("chains"), Chains);
  OutDocument = Document;
  return true;
}

bool ApplyIKRigDocument(UIKRigDefinition &Rig,
                        const TSharedPtr<const FJsonObject> &Document,
                        FMutationStats &OutStats, FString &OutError) {
  if (!ValidateFields(Document,
                      {TEXT("preview_mesh"), TEXT("retarget_root"),
                       TEXT("goals"), TEXT("solvers"), TEXT("solver_order"),
                       TEXT("chains")},
                      TEXT("ik_rig_document"), OutError))
    return false;
  UIKRigController *Controller = UIKRigController::GetController(&Rig);
  if (!Controller) {
    OutError = TEXT("ik_rig_controller_unavailable");
    return false;
  }
  FString PreviewPath;
  if (!Document->TryGetStringField(TEXT("preview_mesh"), PreviewPath)) {
    OutError = TEXT("ik_rig_preview_mesh_missing");
    return false;
  }
  {
    USkeletalMesh *Preview =
        PreviewPath.IsEmpty()
            ? nullptr
            : UeAgentIKAssetOps::LoadSkeletalMesh(PreviewPath);
    if (!PreviewPath.IsEmpty() && !Preview) {
      OutError = TEXT("ik_rig_preview_mesh_not_found:") + PreviewPath;
      return false;
    }
    if (!Controller->SetSkeletalMesh(Preview)) {
      OutError = TEXT("ik_rig_preview_mesh_apply_failed");
      return false;
    }
  }
  FString RetargetRoot;
  if (Document->TryGetStringField(TEXT("retarget_root"), RetargetRoot) &&
      !Controller->SetRetargetRoot(FName(*RetargetRoot))) {
    OutError = TEXT("ik_rig_retarget_root_apply_failed:") + RetargetRoot;
    return false;
  }

  const TSharedPtr<FJsonObject> *Goals = nullptr;
  if (Document->TryGetObjectField(TEXT("goals"), Goals) && Goals &&
      Goals->IsValid()) {
    TSet<FName> Desired;
    for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair : (*Goals)->Values)
      Desired.Add(FName(*Pair.Key));
    TArray<FName> Remove;
    for (UIKRigEffectorGoal *Goal : Controller->GetAllGoals())
      if (Goal && !Desired.Contains(Goal->GoalName))
        Remove.Add(Goal->GoalName);
    for (const FName Name : Remove)
      Controller->RemoveGoal(Name);
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
        OutError = TEXT("ik_rig_goal_invalid:") + Pair.Key;
        return false;
      }
      const FName GoalName(*Pair.Key);
      if (!Controller->GetGoal(GoalName) &&
          Controller->AddNewGoal(GoalName, FName(*BoneName)) != GoalName) {
        OutError = TEXT("ik_rig_goal_create_failed:") + Pair.Key;
        return false;
      }
      const FName RequestedBone(*BoneName);
      if (Controller->GetBoneForGoal(GoalName) != RequestedBone &&
          !Controller->SetGoalBone(GoalName, RequestedBone)) {
        OutError = TEXT("ik_rig_goal_bone_apply_failed:") + Pair.Key;
        return false;
      }
      FTransform Transform = FTransform::Identity;
      const TSharedPtr<FJsonValue> TransformValue =
          GoalObject->TryGetField(TEXT("current_transform"));
      if (TransformValue.IsValid()) {
        if (!DecodeStruct(TBaseStructure<FTransform>::Get(), TransformValue,
                          &Transform, &Rig, OutError))
          return false;
        Controller->SetGoalCurrentTransform(GoalName, Transform);
      }
      const TSharedPtr<FJsonObject> *Properties = nullptr;
      if (GoalObject->TryGetObjectField(TEXT("properties"), Properties) &&
          Properties && Properties->IsValid()) {
        UeAgentAuthoringObjectApply::FOptions ApplyOptions;
        ApplyOptions.bAllowDomainAuthorizedOwners = true;
        UeAgentAuthoringDirectApply::FResult ApplyResult;
        if (!UeAgentDetailsAuthoringService::ApplyObjectProperties(
                Controller->GetGoal(GoalName), *Properties, ApplyOptions,
                ApplyResult, OutError)) {
          OutError = TEXT("ik_rig_goal_properties_apply_failed:") + Pair.Key +
                     TEXT(":") + OutError;
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
      OutError = TEXT("ik_rig_solver_collection_invalid");
      return false;
    }
    while (Controller->GetNumSolvers() > 0)
      Controller->RemoveSolver(Controller->GetNumSolvers() - 1);
    TSet<FString> Seen;
    for (const TSharedPtr<FJsonValue> &IdValue : *SolverOrder) {
      if (!IdValue.IsValid() || IdValue->Type != EJson::String ||
          Seen.Contains(IdValue->AsString())) {
        OutError = TEXT("ik_rig_solver_order_invalid");
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
        OutError = TEXT("ik_rig_solver_missing:") + Id;
        return false;
      }
      UScriptStruct *Struct = LoadObject<UScriptStruct>(nullptr, *StructPath);
      const int32 Index = Struct ? Controller->AddSolver(Struct) : INDEX_NONE;
      FInstancedStruct *Solver = Index != INDEX_NONE
                                     ? Controller->GetSolverStructAtIndex(Index)
                                     : nullptr;
      const TSharedPtr<FJsonValue> SolverValue =
          (*SolverObject)->TryGetField(TEXT("value"));
      if (!Struct || !Solver || !Solver->GetMutableMemory() ||
          !SolverValue.IsValid() ||
          !DecodeStruct(Struct, SolverValue, Solver->GetMutableMemory(), &Rig,
                        OutError)) {
        OutError =
            TEXT("ik_rig_solver_value_invalid:") + Id + TEXT(":") + OutError;
        return false;
      }
      bool bEnabled = true;
      (*SolverObject)->TryGetBoolField(TEXT("enabled"), bEnabled);
      Controller->SetSolverEnabled(Index, bEnabled);
      FString Bone;
      if ((*SolverObject)->TryGetStringField(TEXT("start_bone"), Bone) &&
          !Bone.IsEmpty())
        Controller->SetStartBone(FName(*Bone), Index);
      if ((*SolverObject)->TryGetStringField(TEXT("end_bone"), Bone) &&
          !Bone.IsEmpty())
        Controller->SetEndBone(FName(*Bone), Index);
      ++OutStats.Solvers;
    }
    if (Seen.Num() != (*Solvers)->Values.Num()) {
      OutError = TEXT("ik_rig_solver_order_mismatch");
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
          Pair.Value.IsValid() ? Pair.Value->AsObject() : nullptr;
      FString Start;
      FString End;
      FString Goal;
      if (!Chain.IsValid() ||
          !Chain->TryGetStringField(TEXT("start_bone"), Start) ||
          !Chain->TryGetStringField(TEXT("end_bone"), End) ||
          !Chain->TryGetStringField(TEXT("goal"), Goal)) {
        OutError = TEXT("ik_rig_chain_invalid:") + Pair.Key;
        return false;
      }
      const FName Name(*Pair.Key);
      if (!Controller->GetRetargetChainByName(Name))
        Controller->AddRetargetChain(Name, FName(*Start), FName(*End),
                                     FName(*Goal));
      if (!Controller->SetRetargetChainStartBone(Name, FName(*Start)) ||
          !Controller->SetRetargetChainEndBone(Name, FName(*End)) ||
          !Controller->SetRetargetChainGoal(Name, FName(*Goal))) {
        OutError = TEXT("ik_rig_chain_apply_failed:") + Pair.Key;
        return false;
      }
      ++OutStats.Chains;
    }
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
    Pose->SetObjectField(TEXT("bone_rotations"), Rotations);
    Poses->SetObjectField(Pair.Key.ToString(), Pose);
  }
  SideObject->SetObjectField(TEXT("poses"), Poses);
  return SideObject;
}

bool BuildIKRetargeterDocument(UIKRetargeter &Retargeter,
                               TSharedPtr<FJsonObject> &OutDocument,
                               FString &OutError) {
  UIKRetargeterController *Controller =
      UIKRetargeterController::GetController(&Retargeter);
  if (!Controller) {
    OutError = TEXT("ik_retargeter_controller_unavailable");
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
    OpObject->SetField(TEXT("value"), EncodeStructDeltaFromNativeDefault(
                                          const_cast<UScriptStruct *>(Struct),
                                          Op->GetMemory(), &Retargeter));
    OpObject->SetBoolField(TEXT("enabled"),
                           Controller->GetRetargetOpEnabled(Index));
    const FName ParentName = Controller->GetParentOpByName(FName(*Id));
    OpObject->SetStringField(TEXT("parent"), ParentName.IsNone()
                                                 ? FString()
                                                 : ParentName.ToString());
    Ops->SetObjectField(Id, OpObject);
    OpOrder.Add(MakeShared<FJsonValueString>(Id));
  }
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
      MappingObject->SetStringField(TEXT("op"), OpName.ToString());
      MappingObject->SetStringField(TEXT("target_chain"),
                                    Pair.TargetChainName.ToString());
      MappingObject->SetStringField(TEXT("source_chain"),
                                    Pair.SourceChainName.ToString());
      Mappings->SetObjectField(Id, MappingObject);
    }
  }
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

bool ApplyRetargetPoseSide(UIKRetargeterController &Controller,
                           const ERetargetSourceOrTarget Side,
                           const TSharedPtr<FJsonObject> &SideObject,
                           UIKRetargeter &Owner, FMutationStats &OutStats,
                           FString &OutError) {
  if (!SideObject.IsValid())
    return true;
  FString RootSpace;
  FString RotationSpace;
  if (!SideObject->TryGetStringField(TEXT("root_translation_space"),
                                     RootSpace) ||
      RootSpace != TEXT("retarget_root_global_translation_delta") ||
      !SideObject->TryGetStringField(TEXT("bone_rotation_space"),
                                     RotationSpace) ||
      RotationSpace != TEXT("bone_local_rotation_delta")) {
    OutError = TEXT("ik_retargeter_pose_space_invalid");
    return false;
  }
  const TSharedPtr<FJsonObject> *Poses = nullptr;
  if (!SideObject->TryGetObjectField(TEXT("poses"), Poses) || !Poses ||
      !Poses->IsValid()) {
    OutError = TEXT("ik_retargeter_pose_collection_invalid");
    return false;
  }
  TArray<FName> ExistingNames;
  Controller.GetRetargetPoses(Side).GenerateKeyArray(ExistingNames);
  for (const FName Existing : ExistingNames) {
    if (!(*Poses)->HasField(Existing.ToString()))
      Controller.RemoveRetargetPose(Existing, Side);
  }
  for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair : (*Poses)->Values) {
    const TSharedPtr<FJsonObject> Pose =
        Pair.Value.IsValid() ? Pair.Value->AsObject() : nullptr;
    if (!Pose.IsValid())
      return false;
    const FName PoseName(*Pair.Key);
    if (!Controller.GetRetargetPoses(Side).Contains(PoseName) &&
        Controller.CreateRetargetPose(PoseName, Side).IsNone()) {
      OutError = TEXT("ik_retargeter_pose_create_failed:") + Pair.Key;
      return false;
    }
    if (!Controller.SetCurrentRetargetPose(PoseName, Side)) {
      OutError = TEXT("ik_retargeter_pose_select_failed:") + Pair.Key;
      return false;
    }
    Controller.ResetRetargetPose(PoseName, {}, Side);
    FVector Root = FVector::ZeroVector;
    if (const TSharedPtr<FJsonValue> Value =
            Pose->TryGetField(TEXT("root_translation"));
        Value.IsValid() && !DecodeStruct(TBaseStructure<FVector>::Get(), Value,
                                         &Root, &Owner, OutError))
      return false;
    Controller.SetRootOffsetInRetargetPose(Root, Side);
    const TSharedPtr<FJsonObject> *Rotations = nullptr;
    if (Pose->TryGetObjectField(TEXT("bone_rotations"), Rotations) &&
        Rotations && Rotations->IsValid()) {
      for (const TPair<FString, TSharedPtr<FJsonValue>> &Rotation :
           (*Rotations)->Values) {
        FQuat Quat = FQuat::Identity;
        if (!DecodeStruct(TBaseStructure<FQuat>::Get(), Rotation.Value, &Quat,
                          &Owner, OutError))
          return false;
        Controller.SetRotationOffsetForRetargetPoseBone(FName(*Rotation.Key),
                                                        Quat, Side);
      }
    }
    ++OutStats.Poses;
  }
  FString Current;
  if (SideObject->TryGetStringField(TEXT("current_pose"), Current) &&
      !Controller.SetCurrentRetargetPose(FName(*Current), Side)) {
    OutError = TEXT("ik_retargeter_current_pose_invalid:") + Current;
    return false;
  }
  return true;
}

bool ApplyIKRetargeterDocument(UIKRetargeter &Retargeter,
                               const TSharedPtr<const FJsonObject> &Document,
                               FMutationStats &OutStats, FString &OutError) {
  if (!ValidateFields(Document,
                      {TEXT("rigs"), TEXT("preview_meshes"), TEXT("ops"),
                       TEXT("op_order"), TEXT("mappings"), TEXT("poses")},
                      TEXT("ik_retargeter_document"), OutError))
    return false;
  UIKRetargeterController *Controller =
      UIKRetargeterController::GetController(&Retargeter);
  if (!Controller) {
    OutError = TEXT("ik_retargeter_controller_unavailable");
    return false;
  }
  const TSharedPtr<FJsonObject> *Rigs = nullptr;
  FString SourceRigPath;
  FString TargetRigPath;
  if (!Document->TryGetObjectField(TEXT("rigs"), Rigs) || !Rigs ||
      !Rigs->IsValid() ||
      !(*Rigs)->TryGetStringField(TEXT("source"), SourceRigPath) ||
      !(*Rigs)->TryGetStringField(TEXT("target"), TargetRigPath)) {
    OutError = TEXT("ik_retargeter_rig_collection_invalid");
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
        OutError = TEXT("ik_retargeter_rig_not_found:") + Path;
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
    OutError = TEXT("ik_retargeter_preview_mesh_collection_invalid");
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
        OutError = TEXT("ik_retargeter_preview_mesh_not_found:") + Path;
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
      OutError = TEXT("ik_retargeter_op_collection_invalid");
      return false;
    }
    TArray<FString> OrderedIds;
    TSet<FString> Seen;
    for (const TSharedPtr<FJsonValue> &IdValue : *OpOrder) {
      if (!IdValue.IsValid() || IdValue->Type != EJson::String ||
          Seen.Contains(IdValue->AsString())) {
        OutError = TEXT("ik_retargeter_op_order_invalid");
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
        OutError = TEXT("ik_retargeter_op_invalid:") + Id;
        return false;
      }
      FString Parent;
      (*OpObject)->TryGetStringField(TEXT("parent"), Parent);
      const FName ParentName(*Parent);
      if (!ParentName.IsNone() && !(*Ops)->HasField(ParentName.ToString())) {
        OutError =
            TEXT("ik_retargeter_op_parent_missing:") + Id + TEXT(":") + Parent;
        return false;
      }
      OrderedIds.Add(Id);
    }
    if (Seen.Num() != (*Ops)->Values.Num()) {
      OutError = TEXT("ik_retargeter_op_order_mismatch");
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
          OutError = TEXT("ik_retargeter_op_value_invalid:") + Id + TEXT(":") +
                     OutError;
          return false;
        }
        const FName ResolvedName =
            Controller->SetOpName(FName(*Id), AddedIndex);
        if (ResolvedName.ToString() != Id) {
          OutError = TEXT("ik_retargeter_op_name_not_unique:") + Id;
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
        OutError = TEXT("ik_retargeter_op_parent_cycle");
        return false;
      }
    }
    for (int32 TargetIndex = 0; TargetIndex < OrderedIds.Num(); ++TargetIndex) {
      const int32 CurrentIndex =
          Controller->GetIndexOfOpByName(FName(*OrderedIds[TargetIndex]));
      if (CurrentIndex != TargetIndex &&
          !Controller->MoveRetargetOpInStack(CurrentIndex, TargetIndex)) {
        OutError = TEXT("ik_retargeter_op_order_apply_failed:") +
                   OrderedIds[TargetIndex];
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
        OutError = TEXT("ik_retargeter_op_parent_verify_failed:") + Id;
        return false;
      }
    }
  }

  const TSharedPtr<FJsonObject> *Mappings = nullptr;
  if (Document->TryGetObjectField(TEXT("mappings"), Mappings) && Mappings &&
      Mappings->IsValid()) {
    Controller->AutoMapChains(EAutoMapChainType::Clear, true);
    for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair :
         (*Mappings)->Values) {
      const TSharedPtr<FJsonObject> Mapping =
          Pair.Value.IsValid() ? Pair.Value->AsObject() : nullptr;
      FString Op;
      FString Target;
      FString Source;
      if (!Mapping.IsValid() || !Mapping->TryGetStringField(TEXT("op"), Op) ||
          !Mapping->TryGetStringField(TEXT("target_chain"), Target) ||
          !Mapping->TryGetStringField(TEXT("source_chain"), Source) ||
          Pair.Key != Op + TEXT(":") + Target ||
          !Controller->SetSourceChain(FName(*Source), FName(*Target),
                                      FName(*Op))) {
        OutError = TEXT("ik_retargeter_mapping_invalid:") + Pair.Key;
        return false;
      }
      ++OutStats.Mappings;
    }
  }
  const TSharedPtr<FJsonObject> *Poses = nullptr;
  if (Document->TryGetObjectField(TEXT("poses"), Poses) && Poses &&
      Poses->IsValid()) {
    const TSharedPtr<FJsonObject> *Source = nullptr;
    const TSharedPtr<FJsonObject> *Target = nullptr;
    if ((*Poses)->TryGetObjectField(TEXT("source"), Source) &&
        !ApplyRetargetPoseSide(*Controller, ERetargetSourceOrTarget::Source,
                               Source ? *Source : nullptr, Retargeter, OutStats,
                               OutError))
      return false;
    if ((*Poses)->TryGetObjectField(TEXT("target"), Target) &&
        !ApplyRetargetPoseSide(*Controller, ERetargetSourceOrTarget::Target,
                               Target ? *Target : nullptr, Retargeter, OutStats,
                               OutError))
      return false;
  }
  Controller->CleanAsset();
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
                           FString &OutError) {
  OutError.Reset();
  if (!Outer) {
    OutError = TEXT("rigging_factory_outer_missing");
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
  if (!Result)
    OutError = TEXT("rigging_factory_create_failed:") +
               FString(AssetKindToString(Kind));
  return Result;
}

bool BuildDocument(const EAssetKind Kind, UObject &Asset,
                   TSharedPtr<FJsonObject> &OutDocument, FString &OutError) {
  OutDocument.Reset();
  switch (Kind) {
  case EAssetKind::ControlRig:
    if (UControlRigBlueprint *Blueprint = Cast<UControlRigBlueprint>(&Asset))
      return BuildControlRigDocument(*Blueprint, OutDocument, OutError);
    break;
  case EAssetKind::IKRig:
    if (UIKRigDefinition *Rig = Cast<UIKRigDefinition>(&Asset))
      return BuildIKRigDocument(*Rig, OutDocument, OutError);
    break;
  case EAssetKind::IKRetargeter:
    if (UIKRetargeter *Retargeter = Cast<UIKRetargeter>(&Asset))
      return BuildIKRetargeterDocument(*Retargeter, OutDocument, OutError);
    break;
  default:
    break;
  }
  OutError =
      TEXT("rigging_asset_type_mismatch:") + FString(AssetKindToString(Kind));
  return false;
}

bool ApplyDocument(const EAssetKind Kind, UObject &Asset,
                   const TSharedPtr<const FJsonObject> &Document,
                   FMutationStats &OutStats, FString &OutError) {
  OutStats = FMutationStats();
  OutError.Reset();
  if (!Document.IsValid())
    return true;
  switch (Kind) {
  case EAssetKind::ControlRig:
    if (UControlRigBlueprint *Blueprint = Cast<UControlRigBlueprint>(&Asset))
      return ApplyControlRigDocument(*Blueprint, Document, OutStats, OutError);
    break;
  case EAssetKind::IKRig:
    if (UIKRigDefinition *Rig = Cast<UIKRigDefinition>(&Asset))
      return ApplyIKRigDocument(*Rig, Document, OutStats, OutError);
    break;
  case EAssetKind::IKRetargeter:
    if (UIKRetargeter *Retargeter = Cast<UIKRetargeter>(&Asset))
      return ApplyIKRetargeterDocument(*Retargeter, Document, OutStats,
                                       OutError);
    break;
  default:
    break;
  }
  OutError =
      TEXT("rigging_asset_type_mismatch:") + FString(AssetKindToString(Kind));
  return false;
}

bool CompileAsset(const EAssetKind Kind, UObject &Asset, FString &OutError) {
  OutError.Reset();
  if (Kind == EAssetKind::ControlRig) {
    UControlRigBlueprint *Blueprint = Cast<UControlRigBlueprint>(&Asset);
    if (!Blueprint) {
      OutError = TEXT("control_rig_compile_target_invalid");
      return false;
    }
    FCompilerResultsLog CompilerLog;
    FKismetEditorUtilities::CompileBlueprint(
        Blueprint, EBlueprintCompileOptions::None, &CompilerLog);
    if (CompilerLog.NumErrors > 0 || Blueprint->Status == BS_Error) {
      OutError = TEXT("control_rig_compile_failed");
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
    OutError = TEXT("ik_rig_compile_target_invalid");
    return false;
  }
  UIKRetargeter *Retargeter = Cast<UIKRetargeter>(&Asset);
  if (UIKRetargeterController *Controller =
          Retargeter ? UIKRetargeterController::GetController(Retargeter)
                     : nullptr) {
    Controller->CleanAsset();
    return true;
  }
  OutError = TEXT("ik_retargeter_compile_target_invalid");
  return false;
}

bool SaveAsset(const EAssetKind Kind, UObject &Asset, FString &OutError) {
  return Kind == EAssetKind::ControlRig
             ? UeAgentControlRigOps::SaveAssetPackage(&Asset, OutError)
             : UeAgentIKAssetOps::SaveAssetPackage(&Asset, OutError);
}
} // namespace UeAgentRiggingAuthoring
