// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringDocuments.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringJsonFields.h"
#include "Utilities/AuthoringCore/UeAgentNativeNormalizationIntent.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringEncodedTypes.h"
#include "Utilities/AuthoringCore/UeAgentAnimationSequenceChanges.h"
#include "Misc/ScopeExit.h"
#include "Domains/Animation/Authoring/UeAgentAnimationMontageStructure.h"
#include "Domains/Reflection/Authoring/UeAgentMeshMaterialRelations.h"
#include "Animation/AnimMontage.h"
#include "Factories/AnimMontageFactory.h"
#include "Domains/Animation/Authoring/UeAgentAnimationSequenceStructure.h"
#include "Domains/Animation/Authoring/UeAgentAnimationEvents.h"
#include "Domains/Animation/Authoring/UeAgentAnimationPoseStructure.h"
#include "Animation/PoseAsset.h"
#include "UObject/Class.h"
#include "Factories/PoseAssetFactory.h"

#include "AnimGraphNode_StateMachineBase.h"
#include "AnimStateNodeBase.h"
#include "AnimStateTransitionNode.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace.h"
#include "Animation/BlendSpace1D.h"
#include "Factories/AnimSequenceFactory.h"
#include "Factories/BlendSpaceFactory1D.h"
#include "Factories/BlendSpaceFactoryNew.h"
#include "Animation/MorphTarget.h"
#include "Animation/Skeleton.h"
#include "AnimationStateMachineGraph.h"
#include "BlueprintEditorLibrary.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Domains/Animation/UeAgentAnimationAssetDomainBackend.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringExport.h"
#include "EditorFramework/AssetImportData.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Factories/AnimBlueprintFactory.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAssetLifecycle.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticCompare.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Utilities/UeAgentSkeletonVirtualBones.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

namespace UeAgentAnimationAuthoring {
namespace {
using FCodecOptions = UeAgentPropertyValueCodec::FOptions;

struct FSocketSpec {
  FName Name;
  FName Bone;
  FVector Location = FVector::ZeroVector;
  FRotator Rotation = FRotator::ZeroRotator;
  FVector Scale = FVector::OneVector;
};

struct FVirtualBoneSpec {
  FName Name;
  FName Source;
  FName Target;
};

TSharedPtr<FJsonValue> EncodeStruct(UScriptStruct *Struct, const void *Value,
                                    UObject *Owner = nullptr) {
  return UeAgentPropertyValueCodec::EncodeStructValue(Struct, Value, Owner,
                                                      FCodecOptions());
}

bool DecodeStruct(UScriptStruct *Struct, const TSharedPtr<FJsonValue> &Value,
                  void *OutValue, UObject *Owner, UE::UAI::Contract::FDiagnostic& OutError) {
  FCodecOptions Options;
  Options.bAllowPartialStructFields = true;
  return UeAgentPropertyValueCodec::DecodeStructValue(Struct, Value, OutValue,
                                                      Owner, Options, OutError);
}

UE::UAI::Contract::FDiagnostic DocumentInputFailure(
    const FString& Code, const FString& Reason, const FString& Pointer,
    const TSharedPtr<FJsonValue>& Actual, const FString& Expected) {
  auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("document_validation"));
  Diagnostic.SourceKind = TEXT("related_input");
  Diagnostic.Source.JsonPath = Pointer;
  Diagnostic.ActualValuePresent = Actual.IsValid();
  Diagnostic.ActualValue = Actual;
  Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(Expected);
  return Diagnostic;
}

UE::UAI::Contract::FDiagnostic DocumentFieldFailure(const FString& Code, const FString& Reason,
    const FJsonObject& Object, const TCHAR* Field, const FString& Pointer, const FString& Expected) {
  auto Diagnostic = DocumentInputFailure(Code, Reason, Pointer, Object.TryGetField(Field), Expected);
  UeAgentAuthoringDiagnostics::LocateAuthoringSource(Diagnostic, Object, Field);
  return Diagnostic;
}

UE::UAI::Contract::FDiagnostic AssetTypeFailure(const FString& Code, const EAssetKind Kind,
    const UObject& Asset, const FString& Operation, const FString& CanonicalAssetPath = FString()) {
  const auto* Profile = FindProfile(Kind);
  auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(Code,
      Profile ? Operation + TEXT(" requires an asset of type ") + Profile->TypeId + TEXT(".")
          : TEXT("The animation operation requested an unregistered asset kind."), TEXT("animation_authoring"));
  Diagnostic.SourceKind = TEXT("native_result");
  Diagnostic.InternalContext = Asset.GetPathName();
  Diagnostic.TargetPath = CanonicalAssetPath;
  if (Profile) {
    Diagnostic.ActualValue = MakeShared<FJsonValueString>(Asset.GetClass()->GetPathName());
    Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(Profile->TypeId);
  } else {
    const auto Actual = MakeShared<FJsonObject>();
    Actual->SetStringField(TEXT("enum_type"), TEXT("UeAgentAnimationAuthoring::EAssetKind"));
    Actual->SetNumberField(TEXT("numeric_value"), static_cast<uint8>(Kind));
    Diagnostic.ActualValue = MakeShared<FJsonValueObject>(Actual);
    Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("registered animation asset kind"));
  }
  return Diagnostic;
}

void BindNativeCaptureFailure(UE::UAI::Contract::FDiagnostic& Diagnostic, const UObject& Asset,
    const FString& CanonicalAssetPath) {
  for (auto& Child : Diagnostic.Diagnostics) BindNativeCaptureFailure(Child, Asset, CanonicalAssetPath);
  if (Diagnostic.PublicCause) {
    auto Cause = MakeShared<UE::UAI::Contract::FDiagnostic>(*Diagnostic.PublicCause);
    BindNativeCaptureFailure(*Cause, Asset, CanonicalAssetPath); Diagnostic.PublicCause = MoveTemp(Cause);
  }
  if (Diagnostic.IsEmpty()) return;
  if (Diagnostic.Source.SourceFile.IsEmpty() && (Diagnostic.Source.SourceUnit.IsEmpty() || Diagnostic.Source.InternalJsonPath.IsEmpty()))
    Diagnostic.SourceKind = TEXT("native_result");
  if (Diagnostic.InternalContext.IsEmpty()) Diagnostic.InternalContext = Asset.GetPathName();
  if (!CanonicalAssetPath.IsEmpty() && (Diagnostic.TargetPath.IsEmpty() || Diagnostic.TargetPath == Asset.GetPathName()))
    Diagnostic.TargetPath = CanonicalAssetPath;
}

bool ValidateFields(const TSharedPtr<const FJsonObject> &Object,
                    const TSet<FString> &Allowed, const FString &Prefix,
                    UE::UAI::Contract::FDiagnostic& OutError) {
  if (!Object.IsValid())
    return true;
  for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair : Object->Values) {
    if (!Allowed.Contains(Pair.Key)) {
      TArray<FString> Names = Allowed.Array();
      Names.Sort();
      OutError = DocumentInputFailure(Prefix + TEXT("_unknown_field"),
          FString::Printf(TEXT("Unknown field: %s."), *Pair.Key),
          TEXT("/") + Pair.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")),
          MakeShared<FJsonValueString>(Pair.Key),
          TEXT("field from: ") + FString::Join(Names, TEXT(", ")));
      UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Object, Pair.Key);
      return false;
    }
  }
  return true;
}

bool TryGetObject(const TSharedPtr<const FJsonObject> &Owner,
                  const TCHAR *Field, TSharedPtr<const FJsonObject> &OutObject,
                  UE::UAI::Contract::FDiagnostic& OutError) {
  OutObject.Reset();
  if (!Owner.IsValid() || !Owner->HasField(Field))
    return true;
  const TSharedPtr<FJsonObject> *Object = nullptr;
  if (!Owner->TryGetObjectField(Field, Object) || !Object ||
      !Object->IsValid()) {
    OutError = DocumentInputFailure(TEXT("animation_document_expected_object"),
        FString::Printf(TEXT("Field '%s' must be an object."), Field), TEXT("/") + FString(Field),
        Owner->TryGetField(Field), TEXT("object"));
    return false;
  }
  OutObject = *Object;
  return true;
}

bool CanonicalEqual(const TSharedPtr<FJsonValue> &A,
                    const TSharedPtr<FJsonValue> &B) {
  return UeAgentAuthoringState::MakeStableCanonicalValueKey(A) ==
         UeAgentAuthoringState::MakeStableCanonicalValueKey(B);
}

bool RequireCurrentValue(const TSharedPtr<const FJsonObject> &Requested,
                         const TSharedPtr<FJsonObject> &Current,
                         const TCHAR *Field, const FString &Prefix,
                         UE::UAI::Contract::FDiagnostic& OutError) {
  if (!Requested.IsValid() || !Requested->HasField(Field))
    return true;
  const TSharedPtr<FJsonValue> RequestedValue = Requested->TryGetField(Field);
  const TSharedPtr<FJsonValue> CurrentValue =
      Current.IsValid() ? Current->TryGetField(Field) : nullptr;
  UeAgentAuthoringSemanticCompare::FComparator Comparator;
  if (!UeAgentAuthoringSemanticCompare::FComparatorRegistry::Get().CopyComparator(
          TEXT("uai.compare.canonical_value"), Comparator)) {
    OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_document_comparator_missing"),
        FString::Printf(TEXT("The canonical value comparator required to check read-only field '%s' is not registered."), Field),
        TEXT("animation_authoring"));
    OutError.SourceKind = TEXT("native_result");
    OutError.SubjectId = TEXT("uai.compare.canonical_value");
    OutError.PropertyPath = Field;
    OutError.ActualValue = MakeShared<FJsonValueNull>();
    OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("registered canonical value comparator"));
    return false;
  }
  UeAgentAuthoringState::FStateEntry RequestedEntry;
  UeAgentAuthoringState::FStateEntry CurrentEntry;
  RequestedEntry.Value = RequestedValue;
  CurrentEntry.Value = CurrentValue;
  UeAgentAuthoringEncodedTypes::FScope CapturedTypes;
  if (const auto Definition = CapturedTypes.BuildDefinition(CurrentValue)) {
    CurrentEntry.ComparisonValueDefinition = MakeShared<FJsonValueObject>(Definition);
    RequestedEntry.ComparisonValueDefinition = CurrentEntry.ComparisonValueDefinition;
  }
  bool bEquivalent = false;
  UE::UAI::Contract::FDiagnostic Difference;
  const bool bCompared = Comparator(CurrentEntry, RequestedEntry, bEquivalent, Difference);
  if (!bCompared || !bEquivalent) {
    UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Difference, TEXT("/") + FString(Field)
        .Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")));
    OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(Prefix + TEXT("_read_only_mismatch"), Difference,
        TEXT("document_validation"), bCompared ? FString::Printf(TEXT("Read-only field '%s' differs from the current value."), Field)
                  : FString::Printf(TEXT("Read-only field '%s' could not be compared."), Field));
    OutError.PropertyPath = Field;
    UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Requested, Field);
    return false;
  }
  return true;
}

template <typename T> T *LoadTypedAsset(const FString &Path) {
  return Path.TrimStartAndEnd().IsEmpty()
             ? nullptr
             : Cast<T>(UeAgentAnimationAssetOps::LoadAssetObject(Path));
}

UClass *LoadClass(const FString &Path) {
  if (Path.TrimStartAndEnd().IsEmpty())
    return nullptr;
  if (UClass *Existing = FindObject<UClass>(nullptr, *Path))
    return Existing;
  return LoadObject<UClass>(nullptr, *Path);
}

FString ObjectPath(const UObject *Object) {
  return Object ? Object->GetPathName() : FString();
}

FString RetargetModeToString(const EBoneTranslationRetargetingMode::Type Mode) {
  switch (Mode) {
  case EBoneTranslationRetargetingMode::Animation:
    return TEXT("animation");
  case EBoneTranslationRetargetingMode::Skeleton:
    return TEXT("skeleton");
  case EBoneTranslationRetargetingMode::AnimationScaled:
    return TEXT("animation_scaled");
  case EBoneTranslationRetargetingMode::AnimationRelative:
    return TEXT("animation_relative");
  case EBoneTranslationRetargetingMode::OrientAndScale:
    return TEXT("orient_and_scale");
  default:
    return TEXT("unknown");
  }
}

bool ParseRetargetMode(const FString &Text,
                       EBoneTranslationRetargetingMode::Type &OutMode) {
  FString Normalized = Text.TrimStartAndEnd().ToLower();
  Normalized.ReplaceInline(TEXT("_"), TEXT(""));
  Normalized.ReplaceInline(TEXT("-"), TEXT(""));
  Normalized.ReplaceInline(TEXT(" "), TEXT(""));
  if (Normalized == TEXT("animation"))
    OutMode = EBoneTranslationRetargetingMode::Animation;
  else if (Normalized == TEXT("skeleton"))
    OutMode = EBoneTranslationRetargetingMode::Skeleton;
  else if (Normalized == TEXT("animationscaled"))
    OutMode = EBoneTranslationRetargetingMode::AnimationScaled;
  else if (Normalized == TEXT("animationrelative"))
    OutMode = EBoneTranslationRetargetingMode::AnimationRelative;
  else if (Normalized == TEXT("orientandscale"))
    OutMode = EBoneTranslationRetargetingMode::OrientAndScale;
  else
    return false;
  return true;
}

FString PreviewMethodToString(
    const EPreviewAnimationBlueprintApplicationMethod Method) {
  return Method == EPreviewAnimationBlueprintApplicationMethod::LinkedAnimGraph
             ? TEXT("linked_anim_graph")
             : TEXT("linked_layers");
}

bool ParsePreviewMethod(
    const FString &Text,
    EPreviewAnimationBlueprintApplicationMethod &OutMethod) {
  FString Normalized = Text.TrimStartAndEnd().ToLower();
  Normalized.ReplaceInline(TEXT("_"), TEXT(""));
  Normalized.ReplaceInline(TEXT("-"), TEXT(""));
  Normalized.ReplaceInline(TEXT(" "), TEXT(""));
  if (Normalized.IsEmpty() || Normalized == TEXT("linkedlayers")) {
    OutMethod = EPreviewAnimationBlueprintApplicationMethod::LinkedLayers;
    return true;
  }
  if (Normalized == TEXT("linkedanimgraph")) {
    OutMethod = EPreviewAnimationBlueprintApplicationMethod::LinkedAnimGraph;
    return true;
  }
  return false;
}

TSharedRef<FJsonObject> BuildSocketObject(const USkeletalMeshSocket &Socket,
                                          UObject *Owner) {
  TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
  Result->SetStringField(TEXT("name"), Socket.SocketName.ToString());
  Result->SetStringField(TEXT("bone"), Socket.BoneName.ToString());
  Result->SetField(TEXT("location"),
                   EncodeStruct(TBaseStructure<FVector>::Get(),
                                &Socket.RelativeLocation, Owner));
  Result->SetField(TEXT("rotation"),
                   EncodeStruct(TBaseStructure<FRotator>::Get(),
                                &Socket.RelativeRotation, Owner));
  Result->SetField(TEXT("scale"), EncodeStruct(TBaseStructure<FVector>::Get(),
                                               &Socket.RelativeScale, Owner));
  return Result;
}

bool RecordNativeRows(const TCHAR* Path, const TArray<TSharedPtr<FJsonValue>>& Arguments,
    const TArray<TSharedPtr<FJsonValue>>& Authored,
    std::initializer_list<TPair<const TCHAR*, const TCHAR*>> Kinds, UE::UAI::Contract::FDiagnostic& Error) {
  if (!UeAgentNativeNormalizationIntent::IsValueRecording()) return true;
  TArray<TSharedPtr<FJsonValue>> Source;
  for (int32 Index = 0; Index < Arguments.Num(); ++Index) {
    const auto Row = MakeShared<FJsonObject>();
    for (const auto& Field : Arguments[Index]->AsObject()->Values)
      if (Authored[Index]->AsObject()->HasField(Field.Key)) Row->SetField(Field.Key, Field.Value);
    Source.Add(MakeShared<FJsonValueObject>(Row));
  }
  UeAgentAuthoringTypeDescriptors::FDescriptor Type;
  Type.Kind = TEXT("array"); Type.ElementType = MakeShared<UeAgentAuthoringTypeDescriptors::FDescriptor>();
  Type.ElementType->Kind = TEXT("object");
  const auto Fields = MakeShared<FJsonObject>(), Element = MakeShared<FJsonObject>();
  for (const auto& Kind : Kinds) {
    const auto Field = MakeShared<FJsonObject>(), FieldType = MakeShared<FJsonObject>();
    if (FCString::Strcmp(Kind.Value, TEXT("name_array")) == 0) {
      FieldType->SetStringField(TEXT("kind"), TEXT("array"));
      const auto NameType = MakeShared<FJsonObject>(); NameType->SetStringField(TEXT("kind"), TEXT("name"));
      FieldType->SetObjectField(TEXT("element_type"), NameType);
    } else FieldType->SetStringField(TEXT("kind"), Kind.Value);
    Field->SetObjectField(TEXT("type"), FieldType); Fields->SetObjectField(Kind.Key, Field);
  }
  Element->SetObjectField(TEXT("fields"), Fields);
  const auto ElementDescriptor = MakeShared<FJsonObject>(), Definition = MakeShared<FJsonObject>();
  ElementDescriptor->SetObjectField(TEXT("value_definition"), Element);
  Definition->SetObjectField(TEXT("element_definition"), ElementDescriptor);
  const auto Value = MakeShared<FJsonValueArray>(Source);
  UeAgentAuthoringEncodedTypes::FScope CapturedTypes;
  if (const auto Captured = CapturedTypes.BuildDefinition(Value))
    if (const auto Items = Captured->TryGetField(TEXT("items"))) Definition->SetField(TEXT("items"), Items);
  return UeAgentNativeNormalizationIntent::RecordValue(Path, Value, Value, Error, &Type, MakeShared<FJsonValueObject>(Definition));
}

bool RecordSocketArguments(UObject& Owner, const TArray<FSocketSpec>& Specs,
    const TArray<TSharedPtr<FJsonValue>>& Authored, UE::UAI::Contract::FDiagnostic& Error) {
  if (!UeAgentNativeNormalizationIntent::IsValueRecording()) return true;
  UeAgentAuthoringEncodedTypes::FScope CapturedTypes;
  TArray<TSharedPtr<FJsonValue>> Arguments;
  for (const auto& Spec : Specs) {
    const auto Row = MakeShared<FJsonObject>();
    Row->SetStringField(TEXT("name"), Spec.Name.ToString());
    Row->SetStringField(TEXT("bone"), Spec.Bone.ToString());
    Row->SetField(TEXT("location"), EncodeStruct(TBaseStructure<FVector>::Get(), &Spec.Location, &Owner));
    Row->SetField(TEXT("rotation"), EncodeStruct(TBaseStructure<FRotator>::Get(), &Spec.Rotation, &Owner));
    Row->SetField(TEXT("scale"), EncodeStruct(TBaseStructure<FVector>::Get(), &Spec.Scale, &Owner));
    Arguments.Add(MakeShared<FJsonValueObject>(Row));
  }
  return RecordNativeRows(TEXT("/sockets"), Arguments, Authored,
      {{TEXT("name"), TEXT("name")}, {TEXT("bone"), TEXT("name")}, {TEXT("location"), TEXT("vector3")},
       {TEXT("rotation"), TEXT("rotator")}, {TEXT("scale"), TEXT("vector3")}}, Error);
}

bool ParseSocketObject(const TSharedPtr<FJsonObject> &Object, UObject &Owner,
                       const USkeletalMeshSocket *Existing,
                       FSocketSpec &OutSpec, UE::UAI::Contract::FDiagnostic& OutError) {
  if (!Object.IsValid()) {
    OutError = DocumentInputFailure(TEXT("animation_socket_expected_object"),
        TEXT("A socket definition must be an object."), FString(), nullptr, TEXT("object"));
    return false;
  }
  if (!ValidateFields(Object,
                      {TEXT("name"), TEXT("bone"), TEXT("location"),
                       TEXT("rotation"), TEXT("scale")},
                      TEXT("animation_socket"), OutError)) {
    UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, FString(), true);
    return false;
  }
  FString Name;
  if (!Object->TryGetStringField(TEXT("name"), Name) ||
      Name.TrimStartAndEnd().IsEmpty()) {
    OutError = DocumentInputFailure(TEXT("animation_socket_name_missing"),
        TEXT("A socket requires a non-empty name."), TEXT("/name"),
        Object->TryGetField(TEXT("name")), TEXT("non-empty string"));
    return false;
  }
  OutSpec.Name = FName(*Name.TrimStartAndEnd());
  if (Existing) {
    OutSpec.Bone = Existing->BoneName;
    OutSpec.Location = Existing->RelativeLocation;
    OutSpec.Rotation = Existing->RelativeRotation;
    OutSpec.Scale = Existing->RelativeScale;
  }
  FString Bone;
  if (Object->TryGetStringField(TEXT("bone"), Bone))
    OutSpec.Bone = FName(*Bone.TrimStartAndEnd());
  if (OutSpec.Bone.IsNone()) {
    OutError = DocumentInputFailure(TEXT("animation_socket_bone_missing"),
        FString::Printf(TEXT("Socket '%s' has no bone, and no existing bone can be inherited."), *OutSpec.Name.ToString()),
        Object->HasField(TEXT("bone")) ? TEXT("/bone") : FString(),
        Object->TryGetField(TEXT("bone")), TEXT("non-None bone name"));
    return false;
  }
  auto DecodeIfPresent = [&](const TCHAR *Field, UScriptStruct *Struct,
                             void *Value) -> bool {
    const TSharedPtr<FJsonValue> Json = Object->TryGetField(Field);
    if (!Json.IsValid())
      return true;
    if (!DecodeStruct(Struct, Json, Value, &Owner, OutError)) {
      UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, TEXT("/") + FString(Field));
      return false;
    }
    return true;
  };
  return DecodeIfPresent(TEXT("location"), TBaseStructure<FVector>::Get(),
                         &OutSpec.Location) &&
         DecodeIfPresent(TEXT("rotation"), TBaseStructure<FRotator>::Get(),
                         &OutSpec.Rotation) &&
         DecodeIfPresent(TEXT("scale"), TBaseStructure<FVector>::Get(),
                         &OutSpec.Scale);
}

USkeletalMeshSocket *ApplySocketSpec(UObject &Owner, const FSocketSpec &Spec,
                                    USkeletalMeshSocket *Socket) {
  if (Socket)
    Socket->Modify();
  else
    Socket = NewObject<USkeletalMeshSocket>(&Owner, NAME_None, RF_Transactional);
  Socket->SocketName = Spec.Name;
  Socket->BoneName = Spec.Bone;
  Socket->RelativeLocation = Spec.Location;
  Socket->RelativeRotation = Spec.Rotation;
  Socket->RelativeScale = Spec.Scale;
  return Socket;
}

TSharedRef<FJsonObject> BuildAnimBlueprintSettings(UAnimBlueprint &Blueprint) {
  TSharedRef<FJsonObject> Settings = MakeShared<FJsonObject>();
  Settings->SetStringField(TEXT("parent_class"),
                           ObjectPath(Blueprint.ParentClass));
  Settings->SetStringField(TEXT("target_skeleton"),
                           ObjectPath(Blueprint.TargetSkeleton));
  Settings->SetBoolField(TEXT("template"), Blueprint.bIsTemplate);
  Settings->SetStringField(TEXT("preview_skeletal_mesh"),
                           ObjectPath(Blueprint.GetPreviewMesh()));
  Settings->SetStringField(
      TEXT("preview_animation_blueprint"),
      ObjectPath(Blueprint.GetPreviewAnimationBlueprint()));
  Settings->SetStringField(
      TEXT("preview_application_method"),
      PreviewMethodToString(
          Blueprint.GetPreviewAnimationBlueprintApplicationMethod()));
  Settings->SetStringField(
      TEXT("preview_animation_blueprint_tag"),
      Blueprint.GetPreviewAnimationBlueprintTag().ToString());
  Settings->SetBoolField(TEXT("use_multi_threaded_animation_update"),
                         Blueprint.bUseMultiThreadedAnimationUpdate);
  Settings->SetBoolField(TEXT("warn_about_blueprint_usage"),
                         Blueprint.bWarnAboutBlueprintUsage);
  return Settings;
}

TSharedPtr<FJsonObject>
BuildAnimBlueprintDocument(UAnimBlueprint &Blueprint, UE::UAI::Contract::FDiagnostic& OutError,
                           const FString &CanonicalAssetPath, const bool bForVerification) {
  TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
  Document->SetObjectField(TEXT("settings"), BuildAnimBlueprintSettings(Blueprint));

  TSharedPtr<FJsonObject> BlueprintContext;
  const auto BuildBlueprintState = bForVerification
      ? UeAgentBlueprintAuthoring::BuildReusableVerificationState
      : UeAgentBlueprintAuthoring::BuildReusableProviderExtension;
  if (!BuildBlueprintState(
          Blueprint, BlueprintContext, OutError, CanonicalAssetPath)) {
    return nullptr;
  }
  Document->SetObjectField(TEXT("blueprint"), BlueprintContext);

  TArray<TSharedPtr<FJsonValue>> StateMachines;
  for (UEdGraph *Graph : Blueprint.FunctionGraphs) {
    if (!Graph)
      continue;
    TArray<UAnimGraphNode_StateMachineBase *> Nodes;
    Graph->GetNodesOfClass<UAnimGraphNode_StateMachineBase>(Nodes);
    for (UAnimGraphNode_StateMachineBase *Node : Nodes) {
      if (!Node)
        continue;
      UAnimationStateMachineGraph *MachineGraph = Node->EditorStateMachineGraph;
      TArray<UAnimStateNodeBase *> States;
      TArray<UAnimStateTransitionNode *> Transitions;
      if (MachineGraph) {
        MachineGraph->GetNodesOfClass<UAnimStateNodeBase>(States);
        MachineGraph->GetNodesOfClass<UAnimStateTransitionNode>(Transitions);
      }
      TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
      Item->SetStringField(TEXT("name"), MachineGraph
                                             ? MachineGraph->GetName()
                                             : Node->GetStateMachineName());
      Item->SetStringField(
          TEXT("node_guid"),
          Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
      Item->SetStringField(TEXT("owner_graph"), Graph->GetName());
      Item->SetNumberField(TEXT("state_count"), States.Num());
      Item->SetNumberField(TEXT("transition_count"), Transitions.Num());
      StateMachines.Add(MakeShared<FJsonValueObject>(Item));
    }
  }
  StateMachines.Sort(
      [](const TSharedPtr<FJsonValue> &A, const TSharedPtr<FJsonValue> &B) {
        return A->AsObject()->GetStringField(TEXT("name")) <
               B->AsObject()->GetStringField(TEXT("name"));
      });
  Document->SetArrayField(TEXT("state_machines"), StateMachines);
  return Document;
}

TSharedRef<FJsonObject> BuildSkeletonDocument(USkeleton &Skeleton) {
  TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
  TSharedRef<FJsonObject> Settings = MakeShared<FJsonObject>();
  Settings->SetStringField(TEXT("preview_skeletal_mesh"),
                           ObjectPath(Skeleton.GetPreviewMesh()));
  Settings->SetBoolField(TEXT("use_retarget_modes_from_compatible_skeleton"),
                         Skeleton.GetUseRetargetModesFromCompatibleSkeleton());
  TArray<FString> CompatiblePaths;
  for (const TSoftObjectPtr<USkeleton> &Compatible :
       Skeleton.GetCompatibleSkeletons()) {
    CompatiblePaths.Add(Compatible.ToSoftObjectPath().ToString());
  }
  CompatiblePaths.Sort();
  TArray<TSharedPtr<FJsonValue>> CompatibleValues;
  for (const FString &Path : CompatiblePaths)
    CompatibleValues.Add(MakeShared<FJsonValueString>(Path));
  Settings->SetArrayField(TEXT("compatible_skeletons"), CompatibleValues);
  Document->SetObjectField(TEXT("settings"), Settings);

  TArray<USkeletalMeshSocket *> SortedSockets;
  for (USkeletalMeshSocket *Socket : Skeleton.Sockets)
    if (Socket)
      SortedSockets.Add(Socket);
  SortedSockets.Sort(
      [](const USkeletalMeshSocket &A, const USkeletalMeshSocket &B) {
        return A.SocketName.LexicalLess(B.SocketName);
      });
  TArray<TSharedPtr<FJsonValue>> Sockets;
  for (USkeletalMeshSocket *Socket : SortedSockets)
    Sockets.Add(
        MakeShared<FJsonValueObject>(BuildSocketObject(*Socket, &Skeleton)));
  Document->SetArrayField(TEXT("sockets"), Sockets);

  TArray<TSharedPtr<FJsonValue>> VirtualBones;
  for (const FVirtualBone &Bone : Skeleton.GetVirtualBones()) {
    TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
    Item->SetStringField(TEXT("name"), Bone.VirtualBoneName.ToString());
    Item->SetStringField(TEXT("source"), Bone.SourceBoneName.ToString());
    Item->SetStringField(TEXT("target"), Bone.TargetBoneName.ToString());
    VirtualBones.Add(MakeShared<FJsonValueObject>(Item));
  }
  VirtualBones.Sort(
      [](const TSharedPtr<FJsonValue> &A, const TSharedPtr<FJsonValue> &B) {
        return A->AsObject()->GetStringField(TEXT("name")) <
               B->AsObject()->GetStringField(TEXT("name"));
      });
  Document->SetArrayField(TEXT("virtual_bones"), VirtualBones);

  TArray<TSharedPtr<FJsonValue>> SlotGroups;
  for (const FAnimSlotGroup &Group : Skeleton.GetSlotGroups()) {
    TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
    Item->SetStringField(TEXT("name"), Group.GroupName.ToString());
    TArray<FString> Names;
    for (const FName Slot : Group.SlotNames)
      Names.Add(Slot.ToString());
    Names.Sort();
    TArray<TSharedPtr<FJsonValue>> Slots;
    for (const FString &Name : Names)
      Slots.Add(MakeShared<FJsonValueString>(Name));
    Item->SetArrayField(TEXT("slots"), Slots);
    SlotGroups.Add(MakeShared<FJsonValueObject>(Item));
  }
  SlotGroups.Sort(
      [](const TSharedPtr<FJsonValue> &A, const TSharedPtr<FJsonValue> &B) {
        return A->AsObject()->GetStringField(TEXT("name")) <
               B->AsObject()->GetStringField(TEXT("name"));
      });
  Document->SetArrayField(TEXT("slot_groups"), SlotGroups);

  const FReferenceSkeleton &Ref = Skeleton.GetReferenceSkeleton();
  TArray<TSharedPtr<FJsonValue>> Retargeting;
  TArray<TSharedPtr<FJsonValue>> Bones;
  for (int32 Index = 0; Index < Ref.GetNum(); ++Index) {
    TSharedRef<FJsonObject> Retarget = MakeShared<FJsonObject>();
    Retarget->SetStringField(TEXT("bone"), Ref.GetBoneName(Index).ToString());
    Retarget->SetStringField(
        TEXT("mode"), RetargetModeToString(
                          Skeleton.GetBoneTranslationRetargetingMode(Index)));
    Retargeting.Add(MakeShared<FJsonValueObject>(Retarget));

    const int32 ParentIndex = Ref.GetParentIndex(Index);
    TSharedRef<FJsonObject> Bone = MakeShared<FJsonObject>();
    Bone->SetNumberField(TEXT("index"), Index);
    Bone->SetStringField(TEXT("name"), Ref.GetBoneName(Index).ToString());
    Bone->SetNumberField(TEXT("parent_index"), ParentIndex);
    Bone->SetStringField(TEXT("parent"),
                         ParentIndex == INDEX_NONE
                             ? FString()
                             : Ref.GetBoneName(ParentIndex).ToString());
    const FTransform &Transform = Ref.GetRefBonePose()[Index];
    Bone->SetField(
        TEXT("reference_transform"),
        EncodeStruct(TBaseStructure<FTransform>::Get(), &Transform, &Skeleton));
    Bones.Add(MakeShared<FJsonValueObject>(Bone));
  }
  Document->SetArrayField(TEXT("retargeting"), Retargeting);
  Document->SetArrayField(TEXT("bones"), Bones);
  return Document;
}

TSharedRef<FJsonObject> BuildSkeletalMeshDocument(USkeletalMesh &Mesh) {
  TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
  TSharedRef<FJsonObject> Settings = MakeShared<FJsonObject>();
  Settings->SetStringField(TEXT("skeleton"), ObjectPath(Mesh.GetSkeleton()));
  Settings->SetStringField(TEXT("physics_asset"),
                           ObjectPath(Mesh.GetPhysicsAsset()));
  Settings->SetStringField(TEXT("post_process_anim_blueprint"),
                           ObjectPath(Mesh.GetPostProcessAnimBlueprint()));
  Document->SetObjectField(TEXT("settings"), Settings);

  TArray<TSharedPtr<FJsonValue>> Materials;
  const TArray<FSkeletalMaterial> &MeshMaterials = Mesh.GetMaterials();
  for (int32 Index = 0; Index < MeshMaterials.Num(); ++Index) {
    const FSkeletalMaterial &Material = MeshMaterials[Index];
    TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
    Item->SetNumberField(TEXT("slot_index"), Index);
    Item->SetStringField(TEXT("slot_name"),
                         Material.MaterialSlotName.ToString());
    Item->SetStringField(TEXT("material"),
                         ObjectPath(Material.MaterialInterface));
    Item->SetStringField(TEXT("imported_slot_name"),
                         Material.ImportedMaterialSlotName.ToString());
    Materials.Add(MakeShared<FJsonValueObject>(Item));
  }
  Document->SetArrayField(TEXT("materials"), Materials);

  TArray<USkeletalMeshSocket *> SortedSockets;
  for (USkeletalMeshSocket *Socket : Mesh.GetMeshOnlySocketList())
    if (Socket)
      SortedSockets.Add(Socket);
  SortedSockets.Sort(
      [](const USkeletalMeshSocket &A, const USkeletalMeshSocket &B) {
        return A.SocketName.LexicalLess(B.SocketName);
      });
  TArray<TSharedPtr<FJsonValue>> Sockets;
  for (USkeletalMeshSocket *Socket : SortedSockets)
    Sockets.Add(
        MakeShared<FJsonValueObject>(BuildSocketObject(*Socket, &Mesh)));
  Document->SetArrayField(TEXT("sockets"), Sockets);

  TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
  Summary->SetNumberField(TEXT("lod_count"), Mesh.GetLODNum());
  Summary->SetNumberField(TEXT("bone_count"), Mesh.GetRefSkeleton().GetNum());
  Summary->SetNumberField(TEXT("material_slot_count"), MeshMaterials.Num());
  Summary->SetNumberField(TEXT("socket_count"), SortedSockets.Num());
  Summary->SetNumberField(TEXT("morph_target_count"),
                          Mesh.GetMorphTargets().Num());
  Summary->SetNumberField(TEXT("skin_weight_profile_count"),
                          Mesh.GetNumSkinWeightProfiles());
  Document->SetObjectField(TEXT("summary"), Summary);

  TArray<TSharedPtr<FJsonValue>> Lods;
  TArray<TSharedPtr<FJsonValue>> Sections;
  const FSkeletalMeshModel *ImportedModel = Mesh.GetImportedModel();
  for (int32 LodIndex = 0; LodIndex < Mesh.GetLODNum(); ++LodIndex) {
    TSharedRef<FJsonObject> Lod = MakeShared<FJsonObject>();
    Lod->SetNumberField(TEXT("lod_index"), LodIndex);
    if (const FSkeletalMeshLODInfo *Info = Mesh.GetLODInfo(LodIndex))
      Lod->SetNumberField(TEXT("screen_size"), Info->ScreenSize.Default);
    if (ImportedModel && ImportedModel->LODModels.IsValidIndex(LodIndex)) {
      const FSkeletalMeshLODModel &Model = ImportedModel->LODModels[LodIndex];
      Lod->SetNumberField(TEXT("vertex_count"), Model.NumVertices);
      Lod->SetNumberField(TEXT("triangle_count"), Model.IndexBuffer.Num() / 3);
      Lod->SetNumberField(TEXT("section_count"), Model.Sections.Num());
      for (int32 SectionIndex = 0; SectionIndex < Model.Sections.Num();
           ++SectionIndex) {
        const FSkelMeshSection &Source = Model.Sections[SectionIndex];
        TSharedRef<FJsonObject> Section = MakeShared<FJsonObject>();
        Section->SetNumberField(TEXT("lod_index"), LodIndex);
        Section->SetNumberField(TEXT("section_index"), SectionIndex);
        Section->SetNumberField(TEXT("material_index"), Source.MaterialIndex);
        Section->SetNumberField(TEXT("vertex_count"), Source.GetNumVertices());
        Section->SetNumberField(TEXT("triangle_count"), Source.NumTriangles);
        Section->SetBoolField(TEXT("disabled"), Source.bDisabled);
        Section->SetBoolField(TEXT("casts_shadow"), Source.bCastShadow);
        Sections.Add(MakeShared<FJsonValueObject>(Section));
      }
    }
    Lods.Add(MakeShared<FJsonValueObject>(Lod));
  }
  Document->SetArrayField(TEXT("lods"), Lods);
  Document->SetArrayField(TEXT("sections"), Sections);

  TArray<TSharedPtr<FJsonValue>> MorphTargets;
  for (const TObjectPtr<UMorphTarget> &Target : Mesh.GetMorphTargets()) {
    if (!Target)
      continue;
    TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
    Item->SetStringField(TEXT("name"), Target->GetName());
    Item->SetBoolField(TEXT("has_valid_data"), Target->HasValidData());
    Item->SetNumberField(TEXT("lod_count"), Target->GetMorphLODModels().Num());
    MorphTargets.Add(MakeShared<FJsonValueObject>(Item));
  }
  MorphTargets.Sort(
      [](const TSharedPtr<FJsonValue> &A, const TSharedPtr<FJsonValue> &B) {
        return A->AsObject()->GetStringField(TEXT("name")) <
               B->AsObject()->GetStringField(TEXT("name"));
      });
  Document->SetArrayField(TEXT("morph_targets"), MorphTargets);

  TSharedRef<FJsonObject> ImportData = MakeShared<FJsonObject>();
  const UAssetImportData *AssetImportData = Mesh.GetAssetImportData();
  ImportData->SetStringField(
      TEXT("first_filename"),
      AssetImportData ? AssetImportData->GetFirstFilename() : FString());
  TArray<TSharedPtr<FJsonValue>> SourceFiles;
  if (AssetImportData) {
    TArray<FString> Filenames;
    AssetImportData->ExtractFilenames(Filenames);
    Filenames.Sort();
    for (const FString &Filename : Filenames)
      SourceFiles.Add(MakeShared<FJsonValueString>(Filename));
  }
  ImportData->SetArrayField(TEXT("source_files"), SourceFiles);
  Document->SetObjectField(TEXT("import_data"), ImportData);
  return Document;
}

bool ApplyAnimBlueprintDocument(UAnimBlueprint &Blueprint,
                                const TSharedPtr<const FJsonObject> &Document,
                                FMutationStats &OutStats, UE::UAI::Contract::FDiagnostic& OutError,
                                const FString &CanonicalAssetPath) {
  if (!ValidateFields(
          Document,
          {TEXT("settings"), TEXT("blueprint"), TEXT("state_machines")},
          TEXT("animation_anim_blueprint"), OutError))
    return false;
  TSharedPtr<FJsonObject> Current;
  UeAgentAuthoringEncodedTypes::FScope CapturedTypes;
  if (!BuildDocument(EAssetKind::AnimBlueprint, Blueprint, Current, OutError,
                     CanonicalAssetPath))
    return false;
  if (!RequireCurrentValue(Document, Current, TEXT("blueprint"),
                           TEXT("animation_anim_blueprint"), OutError) ||
      !RequireCurrentValue(Document, Current, TEXT("state_machines"),
                           TEXT("animation_anim_blueprint"), OutError))
    return false;

  TSharedPtr<const FJsonObject> Settings;
  if (!TryGetObject(Document, TEXT("settings"), Settings, OutError) ||
      !Settings.IsValid())
    return OutError.IsEmpty();
  if (!ValidateFields(Settings,
                      {TEXT("parent_class"), TEXT("target_skeleton"),
                       TEXT("template"), TEXT("preview_skeletal_mesh"),
                       TEXT("preview_animation_blueprint"),
                       TEXT("preview_application_method"),
                       TEXT("preview_animation_blueprint_tag"),
                       TEXT("use_multi_threaded_animation_update"),
                       TEXT("warn_about_blueprint_usage")},
                      TEXT("animation_anim_blueprint_settings"), OutError))
    return false;
  const TSharedPtr<FJsonObject> *CurrentSettingsPtr = nullptr;
  Current->TryGetObjectField(TEXT("settings"), CurrentSettingsPtr);
  const TSharedPtr<FJsonObject> CurrentSettings =
      CurrentSettingsPtr ? *CurrentSettingsPtr : nullptr;
  if (!RequireCurrentValue(Settings, CurrentSettings, TEXT("target_skeleton"),
                           TEXT("animation_anim_blueprint_settings"),
                           OutError) ||
      !RequireCurrentValue(Settings, CurrentSettings, TEXT("template"),
                           TEXT("animation_anim_blueprint_settings"),
                           OutError)) {
    return false;
  }

  bool bChanged = false;
  if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(*Settings,
      {{TEXT("parent_class"), EJson::String, true}, {TEXT("preview_skeletal_mesh"), EJson::String, true},
       {TEXT("preview_animation_blueprint"), EJson::String, true}, {TEXT("preview_application_method"), EJson::String},
       {TEXT("preview_animation_blueprint_tag"), EJson::String}, {TEXT("use_multi_threaded_animation_update"), EJson::Boolean},
       {TEXT("warn_about_blueprint_usage"), EJson::Boolean}}, OutError, TEXT("/settings"))) return false;
  auto BeginMutation = [&Blueprint, &bChanged]() {
    if (!bChanged)
      Blueprint.Modify();
    bChanged = true;
  };
  FString Path;
  if (!UeAgentAuthoringJsonFields::ReadReferenceField(*Settings, TEXT("parent_class"), Path, OutError)) return false;
  if (Settings->HasField(TEXT("parent_class")) &&
      !Path.Equals(ObjectPath(Blueprint.ParentClass),
                   ESearchCase::CaseSensitive)) {
    UClass *ParentClass = LoadClass(Path);
    if (!ParentClass || !ParentClass->IsChildOf(UAnimInstance::StaticClass()) ||
        !FKismetEditorUtilities::CanCreateBlueprintOfClass(ParentClass)) {
      OutError = DocumentFieldFailure(TEXT("animation_anim_blueprint_parent_class_invalid"),
          !ParentClass ? TEXT("The parent class could not be loaded.")
              : !ParentClass->IsChildOf(UAnimInstance::StaticClass()) ? TEXT("The parent class must derive from AnimInstance.")
              : TEXT("The native editor does not permit Blueprint creation from this parent class."),
          *Settings, TEXT("parent_class"), TEXT("/settings/parent_class"), TEXT("loadable AnimInstance subclass supported for Blueprint creation"));
      return false;
    }
    BeginMutation();
    UBlueprintEditorLibrary::ReparentBlueprint(&Blueprint, ParentClass);
  }

  if (!UeAgentAuthoringJsonFields::ReadReferenceField(*Settings, TEXT("preview_skeletal_mesh"), Path, OutError)) return false;
  if (Settings->HasField(TEXT("preview_skeletal_mesh")) &&
      !Path.Equals(ObjectPath(Blueprint.GetPreviewMesh()),
                   ESearchCase::CaseSensitive)) {
    USkeletalMesh *Mesh = LoadTypedAsset<USkeletalMesh>(Path);
    if (!Path.IsEmpty() && !Mesh) {
      OutError = DocumentFieldFailure(TEXT("animation_preview_skeletal_mesh_not_found"),
          TEXT("The preview skeletal mesh could not be loaded."), *Settings, TEXT("preview_skeletal_mesh"),
          TEXT("/settings/preview_skeletal_mesh"), TEXT("loadable SkeletalMesh reference"));
      return false;
    }
    if (Mesh && Blueprint.TargetSkeleton && Mesh->GetSkeleton() &&
        !Blueprint.TargetSkeleton->IsCompatibleForEditor(Mesh->GetSkeleton())) {
      OutError = DocumentFieldFailure(TEXT("animation_preview_skeletal_mesh_incompatible"),
          TEXT("The preview mesh skeleton is not compatible with the Animation Blueprint's target skeleton."),
          *Settings, TEXT("preview_skeletal_mesh"), TEXT("/settings/preview_skeletal_mesh"), TEXT("mesh compatible with the target skeleton"));
      return false;
    }
    BeginMutation();
    Blueprint.SetPreviewMesh(Mesh, false);
  }

  if (!UeAgentAuthoringJsonFields::ReadReferenceField(*Settings, TEXT("preview_animation_blueprint"), Path, OutError)) return false;
  if (Settings->HasField(TEXT("preview_animation_blueprint")) &&
      !Path.Equals(ObjectPath(Blueprint.GetPreviewAnimationBlueprint()),
                   ESearchCase::CaseSensitive)) {
    UAnimBlueprint *Preview = LoadTypedAsset<UAnimBlueprint>(Path);
    if (!Path.IsEmpty() && !Preview) {
      OutError = DocumentFieldFailure(TEXT("animation_preview_anim_blueprint_not_found"),
          TEXT("The preview Animation Blueprint could not be loaded."), *Settings, TEXT("preview_animation_blueprint"),
          TEXT("/settings/preview_animation_blueprint"), TEXT("loadable AnimBlueprint reference"));
      return false;
    }
    if (Preview == &Blueprint ||
        (Preview && !Blueprint.IsCompatible(Preview))) {
      OutError = DocumentFieldFailure(TEXT("animation_preview_anim_blueprint_incompatible"),
          Preview == &Blueprint ? TEXT("An Animation Blueprint cannot be its own preview Blueprint.")
              : TEXT("The preview Animation Blueprint is not compatible with this Blueprint."),
          *Settings, TEXT("preview_animation_blueprint"), TEXT("/settings/preview_animation_blueprint"), TEXT("a different compatible AnimBlueprint"));
      return false;
    }
    BeginMutation();
    Blueprint.SetPreviewAnimationBlueprint(Preview);
  }

  FString MethodText;
  if (Settings->TryGetStringField(TEXT("preview_application_method"),
                                  MethodText)) {
    EPreviewAnimationBlueprintApplicationMethod Method;
    if (!ParsePreviewMethod(MethodText, Method)) {
      OutError = DocumentFieldFailure(TEXT("animation_preview_application_method_invalid"),
          TEXT("The preview application method is not supported."), *Settings, TEXT("preview_application_method"),
          TEXT("/settings/preview_application_method"), TEXT("linked_layers or linked_anim_graph"));
      return false;
    }
    if (Method != Blueprint.GetPreviewAnimationBlueprintApplicationMethod()) {
      BeginMutation();
      Blueprint.SetPreviewAnimationBlueprintApplicationMethod(Method);
    }
  }
  FString Tag;
  if (Settings->TryGetStringField(TEXT("preview_animation_blueprint_tag"),
                                  Tag) &&
      FName(*Tag) != Blueprint.GetPreviewAnimationBlueprintTag()) {
    BeginMutation();
    Blueprint.SetPreviewAnimationBlueprintTag(FName(*Tag));
  }
  bool BoolValue = false;
  if (Settings->TryGetBoolField(TEXT("use_multi_threaded_animation_update"),
                                BoolValue) &&
      BoolValue != Blueprint.bUseMultiThreadedAnimationUpdate) {
    BeginMutation();
    Blueprint.bUseMultiThreadedAnimationUpdate = BoolValue;
  }
  if (Settings->TryGetBoolField(TEXT("warn_about_blueprint_usage"),
                                BoolValue) &&
      BoolValue != Blueprint.bWarnAboutBlueprintUsage) {
    BeginMutation();
    Blueprint.bWarnAboutBlueprintUsage = BoolValue;
  }
  if (bChanged) {
    Blueprint.MarkPackageDirty();
    ++OutStats.Settings;
  }
  return true;
}

bool ApplySkeletonSockets(USkeleton &Skeleton,
                          const TArray<TSharedPtr<FJsonValue>> &Values,
                          FMutationStats &OutStats, UE::UAI::Contract::FDiagnostic& OutError) {
  TMap<FName, USkeletalMeshSocket *> ExistingByName;
  for (USkeletalMeshSocket *Socket : Skeleton.Sockets)
    if (Socket)
      ExistingByName.Add(Socket->SocketName, Socket);
  TArray<FSocketSpec> Specs;
  TSet<FName> Names;
  for (int32 RowIndex = 0; RowIndex < Values.Num(); ++RowIndex) {
    const auto& Value = Values[RowIndex];
    const FString RowPointer = TEXT("/") + FString::FromInt(RowIndex);
    if (!Value.IsValid() || Value->Type != EJson::Object) {
      OutError = DocumentInputFailure(TEXT("animation_skeleton_socket_expected_object"),
          TEXT("A skeleton socket definition must be an object."), RowPointer, Value, TEXT("object"));
      return false;
    }
    TSharedPtr<FJsonObject> Object = Value->AsObject();
    FString Name;
    Object->TryGetStringField(TEXT("name"), Name);
    FSocketSpec Spec;
    if (!ParseSocketObject(Object, Skeleton,
                           ExistingByName.FindRef(FName(*Name.TrimStartAndEnd())), Spec,
                           OutError)) {
      UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, RowPointer);
      return false;
    }
    if (Names.Contains(Spec.Name)) {
      OutError = DocumentInputFailure(TEXT("animation_skeleton_socket_duplicate"),
          FString::Printf(TEXT("Socket name '%s' occurs more than once."), *Spec.Name.ToString()),
          RowPointer + TEXT("/name"), Object->TryGetField(TEXT("name")), TEXT("unique socket name"));
      return false;
    }
    if (Skeleton.GetReferenceSkeleton().FindBoneIndex(Spec.Bone) ==
        INDEX_NONE) {
      OutError = DocumentInputFailure(TEXT("animation_skeleton_socket_bone_not_found"),
          FString::Printf(TEXT("Socket '%s' refers to bone '%s', which is absent from the reference skeleton (%d bones)."),
              *Spec.Name.ToString(), *Spec.Bone.ToString(), Skeleton.GetReferenceSkeleton().GetNum()),
          RowPointer + (Object->HasTypedField<EJson::String>(TEXT("bone")) ? TEXT("/bone") : TEXT("")),
          MakeShared<FJsonValueString>(Spec.Bone.ToString()), TEXT("bone in the reference skeleton"));
      return false;
    }
    Names.Add(Spec.Name);
    Specs.Add(Spec);
  }
  if (!RecordSocketArguments(Skeleton, Specs, Values, OutError)) return false;
  Specs.Sort([](const FSocketSpec &A, const FSocketSpec &B) {
    return A.Name.LexicalLess(B.Name);
  });
  Skeleton.Modify();
  Skeleton.Sockets.Reset();
  for (const FSocketSpec &Spec : Specs) {
    Skeleton.Sockets.Add(
        ApplySocketSpec(Skeleton, Spec, ExistingByName.FindRef(Spec.Name)));
  }
  Skeleton.MarkPackageDirty();
  OutStats.Sockets += Specs.Num();
  return true;
}

bool ApplySkeletonDocument(USkeleton &Skeleton,
                           const TSharedPtr<const FJsonObject> &Document,
                           FMutationStats &OutStats, UE::UAI::Contract::FDiagnostic& OutError,
                           const FString& TargetPath) {
  if (!ValidateFields(Document,
                      {TEXT("settings"), TEXT("sockets"), TEXT("virtual_bones"),
                       TEXT("slot_groups"), TEXT("retargeting"), TEXT("bones")},
                      TEXT("animation_skeleton"), OutError)) {
    UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, FString(), true);
    return false;
  }
  TSharedPtr<FJsonObject> Current;
  UeAgentAuthoringEncodedTypes::FScope CapturedTypes;
  if (!BuildDocument(EAssetKind::Skeleton, Skeleton, Current, OutError))
    return false;
  if (!RequireCurrentValue(Document, Current, TEXT("bones"),
                           TEXT("animation_skeleton"), OutError))
    return false;

  TSharedPtr<const FJsonObject> Settings;
  if (!TryGetObject(Document, TEXT("settings"), Settings, OutError))
    return false;
  if (Settings.IsValid()) {
    if (!ValidateFields(Settings,
                        {TEXT("preview_skeletal_mesh"),
                         TEXT("compatible_skeletons"),
                         TEXT("use_retarget_modes_from_compatible_skeleton")},
                        TEXT("animation_skeleton_settings"), OutError)) {
      UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, TEXT("/settings"), true);
      return false;
    }
    FString PreviewPath;
    if (Settings->TryGetStringField(TEXT("preview_skeletal_mesh"),
                                    PreviewPath) &&
        !PreviewPath.Equals(ObjectPath(Skeleton.GetPreviewMesh()),
                            ESearchCase::CaseSensitive)) {
      USkeletalMesh *Preview = LoadTypedAsset<USkeletalMesh>(PreviewPath);
      if (!PreviewPath.IsEmpty() && !Preview) {
        OutError = DocumentInputFailure(TEXT("animation_skeleton_preview_mesh_not_found"),
            TEXT("The preview mesh path could not be loaded as a skeletal mesh."),
            TEXT("/settings/preview_skeletal_mesh"), MakeShared<FJsonValueString>(PreviewPath),
            USkeletalMesh::StaticClass()->GetPathName());
        return false;
      }
      Skeleton.Modify();
      Skeleton.SetPreviewMesh(Preview, false);
      Skeleton.MarkPackageDirty();
      ++OutStats.Settings;
    }
    bool bUseCompatible = false;
    if (Settings->TryGetBoolField(
            TEXT("use_retarget_modes_from_compatible_skeleton"),
            bUseCompatible) &&
        bUseCompatible !=
            Skeleton.GetUseRetargetModesFromCompatibleSkeleton()) {
      Skeleton.Modify();
      Skeleton.SetUseRetargetModesFromCompatibleSkeleton(bUseCompatible);
      Skeleton.MarkPackageDirty();
      ++OutStats.Settings;
    }
    const TArray<TSharedPtr<FJsonValue>> *CompatibleValues = nullptr;
    if (Settings->TryGetArrayField(TEXT("compatible_skeletons"),
                                   CompatibleValues) &&
        CompatibleValues) {
      const TSharedPtr<FJsonValue> CurrentValue =
          Current->GetObjectField(TEXT("settings"))
              ->TryGetField(TEXT("compatible_skeletons"));
      const TSharedPtr<FJsonValue> RequestedValue =
          Settings->TryGetField(TEXT("compatible_skeletons"));
      if (!CanonicalEqual(RequestedValue, CurrentValue)) {
        TArray<USkeleton *> CompatibleAssets;
        TSet<FString> Paths;
        for (int32 RowIndex = 0; RowIndex < CompatibleValues->Num(); ++RowIndex) {
          const auto& Value = (*CompatibleValues)[RowIndex];
          const FString Pointer = TEXT("/settings/compatible_skeletons/") + FString::FromInt(RowIndex);
          if (!Value.IsValid() || Value->Type != EJson::String) {
            OutError = DocumentInputFailure(TEXT("animation_compatible_skeleton_path_expected"),
                TEXT("A compatible skeleton must be specified by an asset path string."),
                Pointer, Value, TEXT("skeleton asset path string"));
            return false;
          }
          const FString Path = Value->AsString();
          USkeleton *Compatible = LoadTypedAsset<USkeleton>(Path);
          if (!Compatible || Compatible == &Skeleton || Paths.Contains(Path)) {
            const FString Reason = !Compatible
                ? TEXT("The compatible skeleton path could not be loaded as a skeleton.")
                : Compatible == &Skeleton ? TEXT("A skeleton cannot list itself as a compatible skeleton.")
                : TEXT("The compatible skeleton path occurs more than once.");
            OutError = DocumentInputFailure(TEXT("animation_compatible_skeleton_invalid"), Reason,
                Pointer, Value, !Compatible ? USkeleton::StaticClass()->GetPathName()
                    : Compatible == &Skeleton ? TEXT("a different skeleton") : TEXT("unique skeleton asset path"));
            return false;
          }
          Paths.Add(Path);
          CompatibleAssets.Add(Compatible);
        }
        Skeleton.Modify();
        const TArray<TSoftObjectPtr<USkeleton>> ExistingCompatibleSkeletons =
            Skeleton.GetCompatibleSkeletons();
        for (const TSoftObjectPtr<USkeleton> &Existing :
             ExistingCompatibleSkeletons)
          Skeleton.RemoveCompatibleSkeleton(Existing);
        for (USkeleton *Compatible : CompatibleAssets)
          Skeleton.AddCompatibleSkeleton(Compatible);
        Skeleton.MarkPackageDirty();
        OutStats.CompatibleSkeletons += CompatibleAssets.Num();
      }
    }
  }

  auto ApplyArrayIfChanged =
      [&](const TCHAR *Field,
          TFunctionRef<bool(const TArray<TSharedPtr<FJsonValue>> &)> Apply)
      -> bool {
    const TArray<TSharedPtr<FJsonValue>> *Values = nullptr;
    if (!Document->TryGetArrayField(Field, Values) || !Values)
      return true;
    if (CanonicalEqual(Document->TryGetField(Field),
                       Current->TryGetField(Field)))
      return true;
    if (Apply(*Values)) return true;
    UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, TEXT("/") + FString(Field));
    return false;
  };
  if (!ApplyArrayIfChanged(
          TEXT("sockets"), [&](const TArray<TSharedPtr<FJsonValue>> &Values) {
            return ApplySkeletonSockets(Skeleton, Values, OutStats, OutError);
          }))
    return false;
  if (!ApplyArrayIfChanged(
          TEXT("virtual_bones"),
          [&](const TArray<TSharedPtr<FJsonValue>> &Values) {
            TArray<FVirtualBoneSpec> Specs;
            TSet<FName> Names;
            for (int32 RowIndex = 0; RowIndex < Values.Num(); ++RowIndex) {
              const auto& Value = Values[RowIndex];
              const FString RowPointer = TEXT("/") + FString::FromInt(RowIndex);
              if (!Value.IsValid() || Value->Type != EJson::Object) {
                OutError = DocumentInputFailure(TEXT("animation_virtual_bone_expected_object"),
                    TEXT("A virtual bone definition must be an object."), RowPointer, Value, TEXT("object"));
                return false;
              }
              TSharedPtr<FJsonObject> Item = Value->AsObject();
              if (!ValidateFields(
                      Item, {TEXT("name"), TEXT("source"), TEXT("target")},
                      TEXT("animation_virtual_bone"), OutError)) {
                UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, RowPointer, true);
                return false;
              }
              FString Name, Source, Target;
              Item->TryGetStringField(TEXT("name"), Name);
              Item->TryGetStringField(TEXT("source"), Source);
              Item->TryGetStringField(TEXT("target"), Target);
              FVirtualBoneSpec Spec{FName(*Name), FName(*Source),
                                    FName(*Target)};
              const bool bSourceValid = !Spec.Source.IsNone()
                  && Skeleton.GetReferenceSkeleton().FindBoneIndex(Spec.Source) != INDEX_NONE;
              const bool bTargetValid = !Spec.Target.IsNone()
                  && Skeleton.GetReferenceSkeleton().FindBoneIndex(Spec.Target) != INDEX_NONE;
              if (!bSourceValid || !bTargetValid || (!Spec.Name.IsNone() && Names.Contains(Spec.Name))) {
                const FString Field = !bSourceValid ? TEXT("source") : !bTargetValid ? TEXT("target") : TEXT("name");
                const FString Reason = Field == TEXT("name")
                    ? FString::Printf(TEXT("Duplicate virtual bone name '%s'."), *Name)
                    : FString::Printf(TEXT("Virtual bone %s '%s' does not resolve in the reference skeleton (%d bones)."),
                        *Field, Field == TEXT("source") ? *Source : *Target, Skeleton.GetReferenceSkeleton().GetNum());
                OutError = DocumentInputFailure(TEXT("animation_virtual_bone_invalid"), Reason,
                    RowPointer + TEXT("/") + Field, Item->TryGetField(Field),
                    Field == TEXT("name") ? TEXT("unique virtual bone name") : TEXT("bone in the reference skeleton"));
                OutError.TargetPath = TargetPath;
                return false;
              }
              if (!Spec.Name.IsNone())
                Names.Add(Spec.Name);
              Specs.Add(Spec);
            }
            if (UeAgentNativeNormalizationIntent::IsValueRecording()) {
              TArray<TSharedPtr<FJsonValue>> Arguments;
              for (const auto& Spec : Specs) {
                const auto Row = MakeShared<FJsonObject>();
                Row->SetStringField(TEXT("name"), Spec.Name.ToString());
                Row->SetStringField(TEXT("source"), Spec.Source.ToString()); Row->SetStringField(TEXT("target"), Spec.Target.ToString());
                Arguments.Add(MakeShared<FJsonValueObject>(Row));
              }
              if (!RecordNativeRows(TEXT("/virtual_bones"), Arguments, Values,
                  {{TEXT("name"), TEXT("name")}, {TEXT("source"), TEXT("name")}, {TEXT("target"), TEXT("name")}}, OutError)) return false;
            }
            Skeleton.Modify();
            TArray<FName> RemoveNames;
            for (const FVirtualBone &Bone : Skeleton.GetVirtualBones())
              RemoveNames.Add(Bone.VirtualBoneName);
            if (!RemoveNames.IsEmpty())
              Skeleton.RemoveVirtualBones(RemoveNames);
            for (int32 Index = 0; Index < Specs.Num(); ++Index) {
              const FVirtualBoneSpec& Spec = Specs[Index];
              FName ResultName = Spec.Name;
              FString Field;
              UeAgentSkeletonVirtualBones::FDiagnosticSource DiagnosticSource;
              DiagnosticSource.TargetPath = TargetPath;
              DiagnosticSource.Operation.JsonPath = TEXT("/");
              DiagnosticSource.Name.JsonPath = Values[Index]->AsObject()->HasField(TEXT("name")) ? TEXT("/name") : TEXT("/");
              DiagnosticSource.TargetBone.JsonPath = TEXT("/target");
              if (!UeAgentSkeletonVirtualBones::TryAdd(Skeleton, Spec.Source, Spec.Target,
                    ResultName, OutError, Field, DiagnosticSource)) {
                UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Values[Index]->AsObject(),
                    TEXT("apply"), TargetPath, Field);
                UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError,
                    TEXT("/") + FString::FromInt(Index));
                return false;
              }
            }
            Skeleton.MarkPackageDirty();
            OutStats.VirtualBones += Specs.Num();
            return true;
          }))
    return false;
  if (!ApplyArrayIfChanged(
          TEXT("slot_groups"),
          [&](const TArray<TSharedPtr<FJsonValue>> &Values) {
            struct FGroup {
              FName Name;
              TArray<FName> Slots;
            };
            TArray<FGroup> Groups;
            TSet<FName> GroupNames, SlotNames;
            for (int32 RowIndex = 0; RowIndex < Values.Num(); ++RowIndex) {
              const auto& Value = Values[RowIndex];
              const FString RowPointer = TEXT("/") + FString::FromInt(RowIndex);
              if (!Value.IsValid() || Value->Type != EJson::Object) {
                OutError = DocumentInputFailure(TEXT("animation_slot_group_expected_object"),
                    TEXT("A slot group definition must be an object."), RowPointer, Value, TEXT("object"));
                return false;
              }
              TSharedPtr<FJsonObject> Item = Value->AsObject();
              if (!ValidateFields(Item, {TEXT("name"), TEXT("slots")},
                                  TEXT("animation_slot_group"), OutError)) {
                UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, RowPointer, true);
                return false;
              }
              FString GroupText;
              const TArray<TSharedPtr<FJsonValue>> *Slots = nullptr;
              if (!Item->TryGetStringField(TEXT("name"), GroupText) || GroupText.IsEmpty()) {
                OutError = DocumentInputFailure(TEXT("animation_slot_group_invalid"),
                    TEXT("A slot group requires a non-empty name."), RowPointer + TEXT("/name"),
                    Item->TryGetField(TEXT("name")), TEXT("non-empty string"));
                return false;
              }
              if (!Item->TryGetArrayField(TEXT("slots"), Slots) || !Slots) {
                OutError = DocumentInputFailure(TEXT("animation_slot_group_invalid"),
                    FString::Printf(TEXT("Slot group '%s' requires a slots array."), *GroupText),
                    RowPointer + TEXT("/slots"), Item->TryGetField(TEXT("slots")), TEXT("array of slot names"));
                return false;
              }
              if (GroupNames.Contains(FName(*GroupText))) {
                OutError = DocumentInputFailure(TEXT("animation_slot_group_invalid"),
                    FString::Printf(TEXT("Slot group name '%s' occurs more than once."), *GroupText),
                    RowPointer + TEXT("/name"), Item->TryGetField(TEXT("name")), TEXT("unique slot group name"));
                return false;
              }
              FGroup Group;
              Group.Name = FName(*GroupText);
              GroupNames.Add(Group.Name);
              for (int32 SlotIndex = 0; SlotIndex < Slots->Num(); ++SlotIndex) {
                const auto& SlotValue = (*Slots)[SlotIndex];
                const FString SlotPointer = RowPointer + TEXT("/slots/") + FString::FromInt(SlotIndex);
                if (!SlotValue.IsValid() || SlotValue->Type != EJson::String) {
                  OutError = DocumentInputFailure(TEXT("animation_slot_name_expected"),
                      FString::Printf(TEXT("Every slot in group '%s' must have a string name."), *GroupText),
                      SlotPointer, SlotValue, TEXT("slot name string"));
                  return false;
                }
                const FName Slot(*SlotValue->AsString());
                if (Slot.IsNone() || SlotNames.Contains(Slot)) {
                  OutError = DocumentInputFailure(TEXT("animation_slot_name_duplicate"),
                      Slot.IsNone() ? FString::Printf(TEXT("A slot in group '%s' resolves to None."), *GroupText)
                          : FString::Printf(TEXT("Slot '%s' is repeated; slot names must be unique across all groups."), *Slot.ToString()),
                      SlotPointer, SlotValue, Slot.IsNone() ? TEXT("non-None slot name") : TEXT("unique slot name across all groups"));
                  return false;
                }
                SlotNames.Add(Slot);
                Group.Slots.Add(Slot);
              }
              Groups.Add(MoveTemp(Group));
            }
            if (UeAgentNativeNormalizationIntent::IsValueRecording()) {
              TArray<TSharedPtr<FJsonValue>> Arguments;
              for (const auto& Group : Groups) {
                const auto Row = MakeShared<FJsonObject>(); Row->SetStringField(TEXT("name"), Group.Name.ToString());
                TArray<TSharedPtr<FJsonValue>> Slots; for (const auto& Slot : Group.Slots) Slots.Add(MakeShared<FJsonValueString>(Slot.ToString()));
                Row->SetArrayField(TEXT("slots"), Slots); Arguments.Add(MakeShared<FJsonValueObject>(Row));
              }
              if (!RecordNativeRows(TEXT("/slot_groups"), Arguments, Values,
                  {{TEXT("name"), TEXT("name")}, {TEXT("slots"), TEXT("name_array")}}, OutError)) return false;
            }
            Skeleton.Modify();
            TArray<FName> ExistingGroups;
            for (const FAnimSlotGroup &Group : Skeleton.GetSlotGroups())
              ExistingGroups.Add(Group.GroupName);
            for (const FName Group : ExistingGroups)
              Skeleton.RemoveSlotGroup(Group);
            for (const FGroup &Group : Groups) {
              Skeleton.AddSlotGroupName(Group.Name);
              for (const FName Slot : Group.Slots) {
                Skeleton.SetSlotGroupName(Slot, Group.Name);
              }
            }
            Skeleton.MarkPackageDirty();
            OutStats.SlotGroups += Groups.Num();
            return true;
          }))
    return false;
  if (!ApplyArrayIfChanged(
          TEXT("retargeting"),
          [&](const TArray<TSharedPtr<FJsonValue>> &Values) {
            TArray<TPair<int32, EBoneTranslationRetargetingMode::Type>> Modes;
            TSet<FName> Bones;
            for (int32 RowIndex = 0; RowIndex < Values.Num(); ++RowIndex) {
              const auto& Value = Values[RowIndex];
              const FString RowPointer = TEXT("/") + FString::FromInt(RowIndex);
              if (!Value.IsValid() || Value->Type != EJson::Object) {
                OutError = DocumentInputFailure(TEXT("animation_retargeting_expected_object"),
                    TEXT("A retargeting definition must be an object."), RowPointer, Value, TEXT("object"));
                return false;
              }
              TSharedPtr<FJsonObject> Item = Value->AsObject();
              if (!ValidateFields(Item, {TEXT("bone"), TEXT("mode")},
                                  TEXT("animation_retargeting"), OutError)) {
                UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, RowPointer, true);
                return false;
              }
              FString BoneText, ModeText;
              Item->TryGetStringField(TEXT("bone"), BoneText);
              Item->TryGetStringField(TEXT("mode"), ModeText);
              const FName Bone(*BoneText);
              const int32 Index =
                  Skeleton.GetReferenceSkeleton().FindBoneIndex(Bone);
              EBoneTranslationRetargetingMode::Type Mode;
              if (Index == INDEX_NONE || Bones.Contains(Bone)) {
                OutError = DocumentInputFailure(TEXT("animation_retargeting_invalid"),
                    Index == INDEX_NONE ? FString::Printf(TEXT("Retargeting bone '%s' is absent from the reference skeleton (%d bones)."),
                        *BoneText, Skeleton.GetReferenceSkeleton().GetNum())
                        : FString::Printf(TEXT("Retargeting bone '%s' occurs more than once."), *BoneText),
                    RowPointer + TEXT("/bone"), Item->TryGetField(TEXT("bone")),
                    Index == INDEX_NONE ? TEXT("bone in the reference skeleton") : TEXT("unique retargeting bone"));
                return false;
              }
              if (!ParseRetargetMode(ModeText, Mode)) {
                OutError = DocumentInputFailure(TEXT("animation_retargeting_invalid"),
                    FString::Printf(TEXT("Retargeting mode for bone '%s' is not a supported translation retargeting mode."), *BoneText),
                    RowPointer + TEXT("/mode"), Item->TryGetField(TEXT("mode")), TEXT("translation retargeting mode"));
                TArray<TSharedPtr<FJsonValue>> Choices;
                for (const auto SupportedMode : {EBoneTranslationRetargetingMode::Animation,
                    EBoneTranslationRetargetingMode::Skeleton, EBoneTranslationRetargetingMode::AnimationScaled,
                    EBoneTranslationRetargetingMode::AnimationRelative, EBoneTranslationRetargetingMode::OrientAndScale})
                  Choices.Add(MakeShared<FJsonValueString>(RetargetModeToString(SupportedMode)));
                OutError.ExpectedValue = MakeShared<FJsonValueArray>(Choices);
                return false;
              }
              Bones.Add(Bone);
              Modes.Emplace(Index, Mode);
            }
            if (UeAgentNativeNormalizationIntent::IsValueRecording()) {
              TArray<TSharedPtr<FJsonValue>> Arguments;
              for (const auto& Mode : Modes) {
                const auto Row = MakeShared<FJsonObject>();
                Row->SetStringField(TEXT("bone"), Skeleton.GetReferenceSkeleton().GetBoneName(Mode.Key).ToString());
                Row->SetStringField(TEXT("mode"), RetargetModeToString(Mode.Value)); Arguments.Add(MakeShared<FJsonValueObject>(Row));
              }
              if (!RecordNativeRows(TEXT("/retargeting"), Arguments, Values,
                  {{TEXT("bone"), TEXT("name")}, {TEXT("mode"), TEXT("string")}}, OutError)) return false;
            }
            Skeleton.Modify();
            for (const TPair<int32, EBoneTranslationRetargetingMode::Type>
                     &Mode : Modes)
              Skeleton.SetBoneTranslationRetargetingMode(Mode.Key, Mode.Value,
                                                         false);
            Skeleton.MarkPackageDirty();
            OutStats.RetargetModes += Modes.Num();
            return true;
          }))
    return false;
  return true;
}

bool ApplySkeletalMeshSockets(USkeletalMesh &Mesh,
                              const TArray<TSharedPtr<FJsonValue>> &Values,
                              FMutationStats &OutStats, UE::UAI::Contract::FDiagnostic& OutError) {
  TMap<FName, USkeletalMeshSocket *> ExistingByName;
  for (USkeletalMeshSocket *Socket : Mesh.GetMeshOnlySocketList())
    if (Socket)
      ExistingByName.Add(Socket->SocketName, Socket);
  TArray<FSocketSpec> Specs;
  TSet<FName> Names;
  for (int32 RowIndex = 0; RowIndex < Values.Num(); ++RowIndex) {
    const auto& Value = Values[RowIndex];
    const FString RowPointer = TEXT("/") + FString::FromInt(RowIndex);
    if (!Value.IsValid() || Value->Type != EJson::Object) {
      OutError = DocumentInputFailure(TEXT("animation_mesh_socket_expected_object"),
          TEXT("A skeletal mesh socket definition must be an object."), RowPointer, Value, TEXT("object"));
      return false;
    }
    TSharedPtr<FJsonObject> Object = Value->AsObject();
    FString Name;
    Object->TryGetStringField(TEXT("name"), Name);
    FSocketSpec Spec;
    if (!ParseSocketObject(Object, Mesh, ExistingByName.FindRef(FName(*Name.TrimStartAndEnd())),
                           Spec, OutError)) {
      UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, RowPointer);
      return false;
    }
    const bool bDuplicate = Names.Contains(Spec.Name);
    if (bDuplicate ||
        Mesh.GetRefSkeleton().FindBoneIndex(Spec.Bone) == INDEX_NONE) {
      OutError = DocumentInputFailure(TEXT("animation_mesh_socket_invalid"),
          bDuplicate ? FString::Printf(TEXT("Socket name '%s' occurs more than once."), *Spec.Name.ToString())
              : FString::Printf(TEXT("Socket '%s' refers to bone '%s', which is absent from the mesh reference skeleton (%d bones)."),
                  *Spec.Name.ToString(), *Spec.Bone.ToString(), Mesh.GetRefSkeleton().GetNum()),
          RowPointer + (bDuplicate ? TEXT("/name") : Object->HasTypedField<EJson::String>(TEXT("bone")) ? TEXT("/bone") : TEXT("")),
          MakeShared<FJsonValueString>(bDuplicate ? Spec.Name.ToString() : Spec.Bone.ToString()),
          bDuplicate ? TEXT("unique socket name") : TEXT("bone in the mesh reference skeleton"));
      return false;
    }
    Names.Add(Spec.Name);
    Specs.Add(Spec);
  }
  if (!RecordSocketArguments(Mesh, Specs, Values, OutError)) return false;
  Specs.Sort([](const FSocketSpec &A, const FSocketSpec &B) {
    return A.Name.LexicalLess(B.Name);
  });
  Mesh.Modify();
  Mesh.GetMeshOnlySocketList().Reset();
  for (const FSocketSpec &Spec : Specs) {
    Mesh.AddSocket(ApplySocketSpec(Mesh, Spec, ExistingByName.FindRef(Spec.Name)),
                   false);
  }
  Mesh.MarkPackageDirty();
  OutStats.Sockets += Specs.Num();
  return true;
}

bool ApplySkeletalMeshDocument(USkeletalMesh &Mesh,
                               const TSharedPtr<const FJsonObject> &Document,
                               FMutationStats &OutStats, UE::UAI::Contract::FDiagnostic& OutError) {
  if (!ValidateFields(Document,
                      {TEXT("settings"), TEXT("materials"), TEXT("sockets"),
                       TEXT("summary"), TEXT("lods"), TEXT("sections"),
                       TEXT("morph_targets"), TEXT("import_data")},
                      TEXT("animation_skeletal_mesh"), OutError))
    return false;
  TSharedPtr<FJsonObject> Current;
  UeAgentAuthoringEncodedTypes::FScope CapturedTypes;
  if (!BuildDocument(EAssetKind::SkeletalMesh, Mesh, Current, OutError))
    return false;
  for (const TCHAR *Field : {TEXT("summary"), TEXT("lods"), TEXT("sections"),
                             TEXT("morph_targets"), TEXT("import_data")})
    if (!RequireCurrentValue(Document, Current, Field,
                             TEXT("animation_skeletal_mesh"), OutError))
      return false;

  TSharedPtr<const FJsonObject> Settings;
  if (!TryGetObject(Document, TEXT("settings"), Settings, OutError))
    return false;
  if (Settings.IsValid()) {
    if (!ValidateFields(Settings,
                        {TEXT("skeleton"), TEXT("physics_asset"),
                         TEXT("post_process_anim_blueprint")},
                        TEXT("animation_skeletal_mesh_settings"), OutError))
      return false;
    const TSharedPtr<FJsonObject> CurrentSettings =
        Current->GetObjectField(TEXT("settings"));
    if (!RequireCurrentValue(Settings, CurrentSettings, TEXT("skeleton"),
                             TEXT("animation_skeletal_mesh_settings"),
                             OutError))
      return false;
    FString Path;
    if (Settings->TryGetStringField(TEXT("physics_asset"), Path) &&
        !Path.Equals(ObjectPath(Mesh.GetPhysicsAsset()),
                     ESearchCase::CaseSensitive)) {
      UPhysicsAsset *Physics = LoadTypedAsset<UPhysicsAsset>(Path);
      if (!Path.IsEmpty() && !Physics) {
        OutError = DocumentFieldFailure(TEXT("animation_physics_asset_not_found"),
            TEXT("The physics asset could not be loaded."), *Settings, TEXT("physics_asset"),
            TEXT("/settings/physics_asset"), TEXT("loadable PhysicsAsset reference"));
        return false;
      }
      Mesh.Modify();
      Mesh.SetPhysicsAsset(Physics);
      Mesh.MarkPackageDirty();
      ++OutStats.References;
    }
    if (Settings->TryGetStringField(TEXT("post_process_anim_blueprint"),
                                    Path) &&
        !Path.Equals(ObjectPath(Mesh.GetPostProcessAnimBlueprint()),
                     ESearchCase::CaseSensitive)) {
      UClass *AnimClass = LoadClass(Path);
      if (!Path.IsEmpty() &&
          (!AnimClass || !AnimClass->IsChildOf(UAnimInstance::StaticClass()))) {
        OutError = DocumentFieldFailure(TEXT("animation_post_process_anim_blueprint_invalid"),
            !AnimClass ? TEXT("The post-process animation class could not be loaded.")
                : TEXT("The post-process animation class must derive from AnimInstance."),
            *Settings, TEXT("post_process_anim_blueprint"), TEXT("/settings/post_process_anim_blueprint"), TEXT("loadable AnimInstance subclass"));
        return false;
      }
      Mesh.Modify();
      Mesh.SetPostProcessAnimBlueprint(AnimClass);
      Mesh.MarkPackageDirty();
      ++OutStats.References;
    }
  }

  const TArray<TSharedPtr<FJsonValue>> *MaterialValues = nullptr;
  if (Document->TryGetArrayField(TEXT("materials"), MaterialValues) &&
      MaterialValues &&
      !CanonicalEqual(Document->TryGetField(TEXT("materials")),
                      Current->TryGetField(TEXT("materials")))) {
    TArray<FSkeletalMaterial> Materials;
    TSet<int32> Indices;
    for (int32 InputIndex = 0; InputIndex < MaterialValues->Num();
         ++InputIndex) {
      const TSharedPtr<FJsonValue> &Value = (*MaterialValues)[InputIndex];
      if (!Value.IsValid() || Value->Type != EJson::Object) {
        OutError = DocumentInputFailure(TEXT("animation_material_expected_object"), TEXT("Each material slot must be an object."),
            TEXT("/materials/") + FString::FromInt(InputIndex), Value, TEXT("material slot object"));
        UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Document);
        return false;
      }
      TSharedPtr<FJsonObject> Item = Value->AsObject();
      if (!ValidateFields(Item,
                          {TEXT("slot_index"), TEXT("slot_name"),
                           TEXT("material"), TEXT("imported_slot_name")},
                          TEXT("animation_material"), OutError)) {
        UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, TEXT("/materials/") + FString::FromInt(InputIndex));
        return false;
      }
      double IndexNumber = InputIndex;
      Item->TryGetNumberField(TEXT("slot_index"), IndexNumber);
      const int32 Index = FMath::RoundToInt(IndexNumber);
      if (Index < 0 || Indices.Contains(Index)) {
        OutError = DocumentInputFailure(TEXT("animation_material_slot_index_invalid"),
            Index < 0 ? TEXT("A material slot index must be non-negative.") : TEXT("The material slot index duplicates an earlier slot."),
            TEXT("/materials/") + FString::FromInt(InputIndex) + TEXT("/slot_index"), MakeShared<FJsonValueNumber>(Index),
            TEXT("unique non-negative slot index"));
        UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Item, TEXT("slot_index"));
        return false;
      }
      Indices.Add(Index);
      if (Materials.Num() <= Index)
        Materials.SetNum(Index + 1);
      if (Mesh.GetMaterials().IsValidIndex(Index))
        Materials[Index] = Mesh.GetMaterials()[Index];
      FString Text;
      if (Item->TryGetStringField(TEXT("slot_name"), Text))
        Materials[Index].MaterialSlotName = FName(*Text);
      if (Item->TryGetStringField(TEXT("imported_slot_name"), Text))
        Materials[Index].ImportedMaterialSlotName = FName(*Text);
      if (Item->TryGetStringField(TEXT("material"), Text)) {
        UMaterialInterface *Material = LoadTypedAsset<UMaterialInterface>(Text);
        if (!Text.IsEmpty() && !Material) {
          OutError = DocumentFieldFailure(TEXT("animation_material_not_found"), TEXT("The material asset could not be loaded."),
              *Item, TEXT("material"), TEXT("/materials/") + FString::FromInt(InputIndex) + TEXT("/material"), TEXT("loadable MaterialInterface reference"));
          return false;
        }
        Materials[Index].MaterialInterface = Material;
      }
    }
    for (int32 Index = 0; Index < Materials.Num(); ++Index)
      if (!Indices.Contains(Index)) {
        OutError = DocumentInputFailure(TEXT("animation_material_slot_index_gap"),
            FString::Printf(TEXT("Material slot %d is missing from the contiguous slot sequence."), Index),
            TEXT("/materials"), MakeShared<FJsonValueNumber>(Index), TEXT("each slot index from zero through the highest supplied index"));
        UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Document);
        OutError.SourceKind = TEXT("related_input");
        return false;
      }
    if (UeAgentNativeNormalizationIntent::IsValueRecording()) {
      TArray<TSharedPtr<FJsonValue>> Arguments;
      for (int32 InputIndex = 0; InputIndex < MaterialValues->Num(); ++InputIndex) {
        double IndexNumber = InputIndex;
        (*MaterialValues)[InputIndex]->AsObject()->TryGetNumberField(TEXT("slot_index"), IndexNumber);
        const int32 Index = FMath::RoundToInt(IndexNumber);
        const auto& Material = Materials[Index];
        const auto Row = MakeShared<FJsonObject>();
        Row->SetNumberField(TEXT("slot_index"), Index);
        Row->SetStringField(TEXT("slot_name"), Material.MaterialSlotName.ToString());
        Row->SetStringField(TEXT("imported_slot_name"), Material.ImportedMaterialSlotName.ToString());
        Row->SetStringField(TEXT("material"), ObjectPath(Material.MaterialInterface));
        Arguments.Add(MakeShared<FJsonValueObject>(Row));
      }
      if (!RecordNativeRows(TEXT("/materials"), Arguments, *MaterialValues,
          {{TEXT("slot_index"), TEXT("int")}, {TEXT("slot_name"), TEXT("name")},
           {TEXT("imported_slot_name"), TEXT("name")}, {TEXT("material"), TEXT("string")}}, OutError)) return false;
    }
    Mesh.Modify();
    // The retained animation document addresses fixed destination slot indices;
    // unlike ordinary ordered rows it does not express movement by row identity.
    TArray<int32> Sources;
    for (int32 Index = 0; Index < Materials.Num(); ++Index)
      Sources.Add(Mesh.GetMaterials().IsValidIndex(Index) ? Index : INDEX_NONE);
    UeAgentReflectionAuthoring::MeshMaterialRelations::ReindexSkeletalSlots(
        Mesh, Mesh.GetMaterials().Num(), Sources);
    Mesh.SetMaterials(Materials);
    Mesh.MarkPackageDirty();
    OutStats.Materials += Materials.Num();
  }

  const TArray<TSharedPtr<FJsonValue>> *SocketValues = nullptr;
  if (Document->TryGetArrayField(TEXT("sockets"), SocketValues) &&
      SocketValues &&
      !CanonicalEqual(Document->TryGetField(TEXT("sockets")),
                      Current->TryGetField(TEXT("sockets")))) {
    if (!ApplySkeletalMeshSockets(Mesh, *SocketValues, OutStats, OutError)) {
      UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, TEXT("/sockets"));
      return false;
    }
  }
  return true;
}
} // namespace

bool ProjectDocumentOriginalIntent(TSharedPtr<FJsonValue>& Desired, UE::UAI::Contract::FDiagnostic& OutError) {
  if (!Desired || Desired->Type != EJson::Object) return true;
  const auto Document = Desired->AsObject();
  const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
  if (Document->TryGetArrayField(TEXT("sockets"), Rows)) {
    for (const auto& Value : *Rows) {
      if (!Value || Value->Type != EJson::Object) continue; // The writer owns validation.
      const auto Row = Value->AsObject();
      for (const TCHAR* Field : {TEXT("name"), TEXT("bone")}) {
        FString Text;
        if (Row->TryGetStringField(Field, Text)) Row->SetStringField(Field, Text.TrimStartAndEnd());
      }
    }
  }
  if (Document->TryGetArrayField(TEXT("retargeting"), Rows)) {
    for (int32 RowIndex = 0; RowIndex < Rows->Num(); ++RowIndex) {
      const auto& Value = (*Rows)[RowIndex];
      if (!Value || Value->Type != EJson::Object) continue;
      FString Text;
      const auto Row = Value->AsObject();
      EBoneTranslationRetargetingMode::Type Mode;
      if (!Row->TryGetStringField(TEXT("mode"), Text)) continue;
      if (!ParseRetargetMode(Text, Mode)) {
        OutError = DocumentFieldFailure(TEXT("animation_retargeting_mode_invalid"), TEXT("The bone translation retargeting mode is not supported."),
            *Row, TEXT("mode"), TEXT("/retargeting/") + FString::FromInt(RowIndex) + TEXT("/mode"),
            TEXT("animation, skeleton, animation_scaled, animation_relative or orient_and_scale"));
        return false;
      }
      Row->SetStringField(TEXT("mode"), RetargetModeToString(Mode));
    }
  }
  if (Document->TryGetArrayField(TEXT("materials"), Rows)) {
    for (const auto& Value : *Rows) {
      if (!Value || Value->Type != EJson::Object) continue;
      const auto Row = Value->AsObject();
      double Index;
      if (Row->TryGetNumberField(TEXT("slot_index"), Index)) Row->SetNumberField(TEXT("slot_index"), FMath::RoundToInt(Index));
      FString Reference;
      if (Row->TryGetStringField(TEXT("material"), Reference) && !Reference.IsEmpty())
        Row->SetStringField(TEXT("material"), ToObjectPath(Reference));
    }
  }
  return true;
}

FString NormalizePackagePath(const FString &InPath) {
  FString Result = InPath.TrimStartAndEnd();
  int32 Dot = INDEX_NONE;
  if (Result.FindChar(TEXT('.'), Dot) && Dot > 0)
    Result = Result.Left(Dot);
  return Result;
}

FString ToObjectPath(const FString &PackagePath) {
  const FString Normalized = NormalizePackagePath(PackagePath);
  const FString Name = FPackageName::GetLongPackageAssetName(Normalized);
  return Name.IsEmpty() ? FString()
                        : FString::Printf(TEXT("%s.%s"), *Normalized, *Name);
}

UObject *LoadAsset(const EAssetKind Kind, const FString &InPath) {
  UObject *Object = UeAgentAnimationAssetOps::LoadAssetObject(InPath);
  switch (Kind) {
  case EAssetKind::AnimBlueprint:
    return Cast<UAnimBlueprint>(Object);
  case EAssetKind::Skeleton:
    return Cast<USkeleton>(Object);
  case EAssetKind::SkeletalMesh:
    return Cast<USkeletalMesh>(Object);
  case EAssetKind::AnimSequence:
    return Cast<UAnimSequence>(Object);
  case EAssetKind::AnimMontage:
    return Cast<UAnimMontage>(Object);
  case EAssetKind::BlendSpace:
    return Cast<UBlendSpace>(Object);
  case EAssetKind::PoseAsset:
    return Cast<UPoseAsset>(Object);
  default:
    return nullptr;
  }
}

UObject *CreateFactorySeed(const EAssetKind Kind, UObject *Outer,
                           const FName Name, const EObjectFlags Flags,
                           const TSharedPtr<const FJsonObject> &SeedDocument,
                           UE::UAI::Contract::FDiagnostic& OutError,
                           const FFactorySeedDiagnosticSource& DiagnosticSource) {
  OutError.Reset();
  auto LocateSeedFailure = [&](const UE::UAI::Contract::FSourceLocation& Source) {
    OutError.TargetPath = DiagnosticSource.TargetPath;
    OutError.Source = Source;
    OutError.SourceKind = !Source.SourceFile.IsEmpty() || !Source.JsonPath.IsEmpty()
        ? DiagnosticSource.InputSourceKind : TEXT("native_result");
  };
  auto SeedValueFailure = [&](const TCHAR* Code, const FString& Reason, const TCHAR* Field,
      const FString& Expected, const UE::UAI::Contract::FSourceLocation& Source) {
    OutError = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("animation_authoring"));
    OutError.ActualValue = SeedDocument ? SeedDocument->TryGetField(Field) : nullptr;
    OutError.ActualValuePresent = OutError.ActualValue.IsValid();
    OutError.ExpectedValue = MakeShared<FJsonValueString>(Expected);
    LocateSeedFailure(Source);
  };
  auto SettingsSource = [&](const TCHAR* Field) {
    auto Source = DiagnosticSource.Settings;
    if (Field && *Field) {
      if (!Source.JsonPath.IsEmpty())
        Source.JsonPath = (Source.JsonPath == TEXT("/") ? FString() : Source.JsonPath)
            + TEXT("/") + Field;
      if (!Source.InternalJsonPath.IsEmpty())
        Source.InternalJsonPath = (Source.InternalJsonPath == TEXT("/") ? FString() : Source.InternalJsonPath)
            + TEXT("/") + Field;
      // A root location is not a field location; the input owner resolves the leaf.
      Source.Line = 0;
      Source.Column = 0;
    }
    return Source;
  };
  auto ReadSeedSettings = [&](TSharedPtr<const FJsonObject>& Settings) {
    if (TryGetObject(SeedDocument, TEXT("settings"), Settings, OutError)) return true;
    LocateSeedFailure(SettingsSource(nullptr));
    return false;
  };
  auto SettingsValueFailure = [&](const TCHAR* Code, const FString& Reason,
      const TSharedPtr<const FJsonObject>& Settings, const TCHAR* Field,
      const FString& Expected) {
    OutError = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("animation_authoring"));
    OutError.PropertyPath = Field;
    OutError.ActualValue = Settings ? Settings->TryGetField(Field) : nullptr;
    OutError.ActualValuePresent = OutError.ActualValue.IsValid();
    OutError.ExpectedValue = MakeShared<FJsonValueString>(Expected);
    LocateSeedFailure(SettingsSource(Field));
  };
  auto FinishSeed = [&](UObject* Asset, const UClass* ExpectedClass) -> UObject* {
    if (!Asset) {
      OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_factory_seed_create_failed"),
          TEXT("The Unreal animation factory did not create the requested asset."), TEXT("animation_authoring"));
      OutError.ActualValue = MakeShared<FJsonValueNull>();
      OutError.ExpectedValue = MakeShared<FJsonValueString>(ExpectedClass->GetName());
      OutError.ExpectedEvidence = MakeShared<FJsonValueString>(ExpectedClass->GetPathName());
      LocateSeedFailure({});
    }
    return Asset;
  };
  if (!Outer) {
    OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_factory_seed_outer_missing"),
        TEXT("The animation factory requires an owning object or package."), TEXT("animation_authoring"));
    OutError.ActualValue = MakeShared<FJsonValueNull>();
    OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("owning object or package"));
    LocateSeedFailure({});
    return nullptr;
  }
  if (Kind == EAssetKind::Skeleton)
    return FinishSeed(NewObject<USkeleton>(Outer, Name, Flags), USkeleton::StaticClass());
  if (Kind == EAssetKind::AnimSequence || Kind == EAssetKind::BlendSpace) {
    FString SkeletonPath;
    if (SeedDocument && SeedDocument->HasField(TEXT("skeleton")) &&
        !SeedDocument->TryGetStringField(TEXT("skeleton"), SkeletonPath)) {
      SeedValueFailure(TEXT("animation_seed_skeleton_invalid"), TEXT("The skeleton reference must be a string."),
          TEXT("skeleton"), TEXT("a skeleton asset reference string"), DiagnosticSource.Skeleton);
      return nullptr;
    }
    USkeleton* Skeleton = LoadTypedAsset<USkeleton>(SkeletonPath);
    if ((!SkeletonPath.IsEmpty() || Kind == EAssetKind::BlendSpace) && !Skeleton) {
      SeedValueFailure(TEXT("animation_seed_skeleton_not_found"),
          FString::Printf(TEXT("Skeleton reference '%s' could not be loaded as a Skeleton asset."), *SkeletonPath),
          TEXT("skeleton"), USkeleton::StaticClass()->GetPathName(), DiagnosticSource.Skeleton);
      return nullptr;
    }
    if (Kind == EAssetKind::AnimSequence) {
      auto* Factory = NewObject<UAnimSequenceFactory>();
      Factory->TargetSkeleton = Skeleton;
      return FinishSeed(Factory->FactoryCreateNew(UAnimSequence::StaticClass(), Outer, Name, Flags, nullptr, GWarn), UAnimSequence::StaticClass());
    }
    int32 Dimension = 2;
    if (SeedDocument && SeedDocument->HasField(TEXT("dimension")) &&
        !UeAgentAuthoringJsonFields::ReadInt32Field(*SeedDocument, TEXT("dimension"), Dimension, OutError)) {
      LocateSeedFailure(DiagnosticSource.Dimension);
      return nullptr;
    }
    if (Dimension == 1) {
      auto* Factory = NewObject<UBlendSpaceFactory1D>();
      Factory->TargetSkeleton = Skeleton;
      return FinishSeed(Factory->FactoryCreateNew(UBlendSpace1D::StaticClass(), Outer, Name, Flags, nullptr, GWarn), UBlendSpace1D::StaticClass());
    }
    if (Dimension != 2) {
      SeedValueFailure(TEXT("animation_blend_space_dimension_invalid"), TEXT("BlendSpace dimension must be 1 or 2."),
          TEXT("dimension"), TEXT("1 or 2"), DiagnosticSource.Dimension);
      return nullptr;
    }
    auto* Factory = NewObject<UBlendSpaceFactoryNew>();
    Factory->TargetSkeleton = Skeleton;
    return FinishSeed(Factory->FactoryCreateNew(UBlendSpace::StaticClass(), Outer, Name, Flags, nullptr, GWarn), UBlendSpace::StaticClass());
  }
  if (Kind == EAssetKind::PoseAsset) {
    TSharedPtr<const FJsonObject> Settings;
    if (!ReadSeedSettings(Settings)) return nullptr;
    FString Source;
    if (!Settings || !Settings->TryGetStringField(TEXT("source_animation"), Source)) {
      SettingsValueFailure(TEXT("animation_pose_source_animation_required"),
          TEXT("Creating a PoseAsset requires a source_animation asset reference string."),
          Settings, TEXT("source_animation"), TEXT("AnimSequence asset reference string"));
      return nullptr;
    }
    auto* Factory = NewObject<UPoseAssetFactory>();
    Factory->SourceAnimation = LoadTypedAsset<UAnimSequence>(Source);
    if (!Factory->SourceAnimation || !Factory->SourceAnimation->GetSkeleton()) {
      SettingsValueFailure(TEXT("animation_pose_source_animation_invalid"),
          !Factory->SourceAnimation
              ? TEXT("The PoseAsset source_animation reference could not be loaded as an AnimSequence.")
              : TEXT("The PoseAsset source AnimSequence has no Skeleton."),
          Settings, TEXT("source_animation"), TEXT("AnimSequence with a Skeleton"));
      return nullptr;
    }
    return FinishSeed(Factory->FactoryCreateNew(UPoseAsset::StaticClass(), Outer, Name, Flags, nullptr, GWarn), UPoseAsset::StaticClass());
  }
  if (Kind == EAssetKind::AnimMontage) {
    TSharedPtr<const FJsonObject> Settings;
    if (!ReadSeedSettings(Settings)) return nullptr;
    auto* Factory = NewObject<UAnimMontageFactory>();
    FString Path;
    if (Settings && Settings->TryGetStringField(TEXT("target_skeleton"), Path)) {
      Factory->TargetSkeleton = LoadTypedAsset<USkeleton>(Path);
      if (!Factory->TargetSkeleton) {
        SettingsValueFailure(TEXT("animation_montage_skeleton_not_found"),
            TEXT("The montage target_skeleton reference could not be loaded as a Skeleton."),
            Settings, TEXT("target_skeleton"), TEXT("Skeleton asset reference"));
        return nullptr;
      }
    }
    if (Settings && Settings->TryGetStringField(TEXT("source_animation"), Path)) {
      Factory->SourceAnimation = LoadTypedAsset<UAnimSequence>(Path);
      if (!Factory->SourceAnimation) {
        SettingsValueFailure(TEXT("animation_montage_source_not_found"),
            TEXT("The montage source_animation reference could not be loaded as an AnimSequence."),
            Settings, TEXT("source_animation"), TEXT("AnimSequence asset reference"));
        return nullptr;
      }
    }
    if (!Factory->TargetSkeleton && Factory->SourceAnimation) Factory->TargetSkeleton = Factory->SourceAnimation->GetSkeleton();
    if (!Factory->TargetSkeleton || (Factory->SourceAnimation && Factory->SourceAnimation->GetSkeleton() != Factory->TargetSkeleton)) {
      SettingsValueFailure(TEXT("animation_montage_factory_skeleton_invalid"),
          !Factory->TargetSkeleton
              ? TEXT("Creating a montage requires target_skeleton or a source AnimSequence with a Skeleton.")
              : TEXT("The montage source AnimSequence Skeleton does not match target_skeleton."),
          Settings, Factory->SourceAnimation ? TEXT("source_animation") : TEXT("target_skeleton"),
          Factory->TargetSkeleton ? Factory->TargetSkeleton->GetPathName() : TEXT("Skeleton asset reference"));
      if (Factory->SourceAnimation) {
        const USkeleton* SourceSkeleton = Factory->SourceAnimation->GetSkeleton();
        if (SourceSkeleton) OutError.ActualValue = MakeShared<FJsonValueString>(SourceSkeleton->GetPathName());
        else OutError.ActualValue = MakeShared<FJsonValueNull>();
        OutError.ActualValuePresent = true;
      }
      return nullptr;
    }
    UAnimMontage* Montage = Cast<UAnimMontage>(Factory->FactoryCreateNew(
        UAnimMontage::StaticClass(), Outer, Name, Flags, nullptr, GWarn));
    // Match the native loaded state before capturing the proposed document.
    if (Montage) Montage->InitializeNotifyTrack();
    return FinishSeed(Montage, UAnimMontage::StaticClass());
  }
  if (Kind == EAssetKind::SkeletalMesh) {
    USkeletalMesh *Mesh = NewObject<USkeletalMesh>(Outer, Name, Flags);
    Mesh->AllocateResourceForRendering();
    TSharedPtr<const FJsonObject> Settings;
    if (!ReadSeedSettings(Settings))
      return nullptr;
    FString SkeletonPath;
    if (Settings.IsValid() &&
        Settings->TryGetStringField(TEXT("skeleton"), SkeletonPath)) {
      USkeleton *Skeleton = LoadTypedAsset<USkeleton>(SkeletonPath);
      if (!SkeletonPath.IsEmpty() && !Skeleton) {
        SettingsValueFailure(TEXT("animation_skeletal_mesh_seed_skeleton_not_found"),
            TEXT("The skeletal mesh skeleton reference could not be loaded as a Skeleton."),
            Settings, TEXT("skeleton"), TEXT("Skeleton asset reference"));
        return nullptr;
      }
      Mesh->SetSkeleton(Skeleton);
    }
    return Mesh;
  }
  if (Kind != EAssetKind::AnimBlueprint) {
    OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_factory_seed_kind_unsupported"),
        TEXT("The animation factory does not support the requested asset kind."), TEXT("animation_authoring"));
    OutError.PropertyPath = TEXT("asset_kind");
    OutError.ActualValue = MakeShared<FJsonValueNumber>(static_cast<int32>(Kind));
    OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("Skeleton, AnimSequence, BlendSpace, PoseAsset, AnimMontage, SkeletalMesh or AnimBlueprint"));
    LocateSeedFailure({});
    return nullptr;
  }

  UAnimBlueprintFactory *Factory = NewObject<UAnimBlueprintFactory>();
  if (!Factory) {
    OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_anim_blueprint_factory_create_failed"),
        TEXT("Unreal did not create the AnimBlueprint factory object."), TEXT("animation_authoring"));
    OutError.ActualValue = MakeShared<FJsonValueNull>();
    OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("AnimBlueprintFactory"));
    LocateSeedFailure({});
    return nullptr;
  }
  TSharedPtr<const FJsonObject> Settings;
  if (!ReadSeedSettings(Settings))
    return nullptr;
  if (Settings.IsValid()) {
    FString Path;
    if (Settings->TryGetStringField(TEXT("parent_class"), Path)) {
      UClass* const LoadedParentClass = LoadClass(Path);
      Factory->ParentClass = LoadedParentClass;
      if (!Factory->ParentClass ||
          !Factory->ParentClass->IsChildOf(UAnimInstance::StaticClass()) ||
          !FKismetEditorUtilities::CanCreateBlueprintOfClass(
              Factory->ParentClass)) {
        SettingsValueFailure(TEXT("animation_anim_blueprint_seed_parent_invalid"),
            !LoadedParentClass ? TEXT("The AnimBlueprint parent_class reference could not be loaded as a class.")
                : !LoadedParentClass->IsChildOf(UAnimInstance::StaticClass())
                    ? TEXT("The AnimBlueprint parent_class must derive from AnimInstance.")
                    : TEXT("Unreal does not allow creating a Blueprint from the selected parent_class."),
            Settings, TEXT("parent_class"), TEXT("Blueprint-creatable subclass of AnimInstance"));
        return nullptr;
      }
    }
    Settings->TryGetBoolField(TEXT("template"), Factory->bTemplate);
    if (Settings->TryGetStringField(TEXT("preview_skeletal_mesh"), Path)) {
      Factory->PreviewSkeletalMesh = LoadTypedAsset<USkeletalMesh>(Path);
      if (!Path.IsEmpty() && !Factory->PreviewSkeletalMesh) {
        SettingsValueFailure(TEXT("animation_anim_blueprint_seed_preview_mesh_not_found"),
            TEXT("The AnimBlueprint preview_skeletal_mesh reference could not be loaded as a SkeletalMesh."),
            Settings, TEXT("preview_skeletal_mesh"), TEXT("SkeletalMesh asset reference"));
        return nullptr;
      }
    }
    if (Settings->TryGetStringField(TEXT("target_skeleton"), Path)) {
      Factory->TargetSkeleton = LoadTypedAsset<USkeleton>(Path);
      if (!Path.IsEmpty() && !Factory->TargetSkeleton) {
        SettingsValueFailure(TEXT("animation_anim_blueprint_seed_skeleton_not_found"),
            TEXT("The AnimBlueprint target_skeleton reference could not be loaded as a Skeleton."),
            Settings, TEXT("target_skeleton"), TEXT("Skeleton asset reference"));
        return nullptr;
      }
    }
    if (!Factory->TargetSkeleton && Factory->PreviewSkeletalMesh)
      Factory->TargetSkeleton = Factory->PreviewSkeletalMesh->GetSkeleton();
    if (Factory->bTemplate &&
        (Factory->TargetSkeleton || Factory->PreviewSkeletalMesh)) {
      SettingsValueFailure(TEXT("animation_anim_blueprint_template_seed_has_skeleton"),
          TEXT("A template AnimBlueprint cannot specify a target Skeleton or preview SkeletalMesh."),
          Settings, TEXT("template"), TEXT("false when target_skeleton or preview_skeletal_mesh is assigned"));
      auto ActualSettings = MakeShared<FJsonObject>();
      ActualSettings->SetBoolField(TEXT("template"), Factory->bTemplate);
      ActualSettings->SetBoolField(TEXT("target_skeleton_resolved"), Factory->TargetSkeleton != nullptr);
      ActualSettings->SetBoolField(TEXT("preview_skeletal_mesh_resolved"), Factory->PreviewSkeletalMesh != nullptr);
      OutError.ActualValue = MakeShared<FJsonValueObject>(ActualSettings);
      return nullptr;
    }
  }
  Factory->BlueprintType = BPTYPE_Normal;
  return FinishSeed(Factory->FactoryCreateNew(UAnimBlueprint::StaticClass(), Outer, Name,
                                   Flags, nullptr, GWarn,
                                   FName(TEXT("UeAgentInterface"))), UAnimBlueprint::StaticClass());
}

bool BuildAnimBlueprintSettingsDocument(UObject &Asset,
                                       TSharedPtr<FJsonObject> &OutDocument,
                                       UE::UAI::Contract::FDiagnostic& OutError) {
  OutDocument.Reset();
  OutError.Reset();
  UAnimBlueprint *Blueprint = Cast<UAnimBlueprint>(&Asset);
  if (!Blueprint) {
    OutError = AssetTypeFailure(TEXT("animation_anim_blueprint_settings_type_invalid"), EAssetKind::AnimBlueprint,
        Asset, TEXT("Animation Blueprint settings capture"));
    return false;
  }
  OutDocument = MakeShared<FJsonObject>();
  OutDocument->SetObjectField(TEXT("settings"), BuildAnimBlueprintSettings(*Blueprint));
  return true;
}

static bool BuildDocumentForPurpose(const EAssetKind Kind, UObject &Asset,
                   TSharedPtr<FJsonObject> &OutDocument, UE::UAI::Contract::FDiagnostic& OutError,
                   const FString &CanonicalAssetPath, const bool bForVerification) {
  ON_SCOPE_EXIT { if (!OutDocument.IsValid()) BindNativeCaptureFailure(OutError, Asset, CanonicalAssetPath); };
  OutDocument.Reset();
  OutError.Reset();
  switch (Kind) {
  case EAssetKind::PoseAsset:
    if (auto* Pose = Cast<UPoseAsset>(&Asset)) OutDocument = ReadPoseStructure(*Pose, OutError);
    break;
  case EAssetKind::AnimBlueprint:
    if (UAnimBlueprint *Blueprint = Cast<UAnimBlueprint>(&Asset))
      OutDocument =
          BuildAnimBlueprintDocument(*Blueprint, OutError, CanonicalAssetPath, bForVerification);
    break;
  case EAssetKind::Skeleton:
    if (USkeleton *Skeleton = Cast<USkeleton>(&Asset))
      OutDocument = BuildSkeletonDocument(*Skeleton);
    else {
      OutError = AssetTypeFailure(TEXT("animation_document_asset_type_mismatch"), Kind, Asset,
          TEXT("Skeleton document capture"), CanonicalAssetPath);
      OutError.Stage = TEXT("document_capture");
    }
    break;
  case EAssetKind::SkeletalMesh:
    if (USkeletalMesh *Mesh = Cast<USkeletalMesh>(&Asset))
      OutDocument = BuildSkeletalMeshDocument(*Mesh);
    break;
  case EAssetKind::AnimSequence:
    if (UAnimSequence* Sequence = Cast<UAnimSequence>(&Asset))
      OutDocument = ReadSequenceStructure(*Sequence, OutError);
    break;
  case EAssetKind::AnimMontage:
    if (auto* Montage = Cast<UAnimMontage>(&Asset)) OutDocument = ReadMontageStructure(*Montage, OutError);
    break;
  case EAssetKind::BlendSpace:
    if (auto* BlendSpace = Cast<UBlendSpace>(&Asset)) {
      OutDocument = MakeShared<FJsonObject>();
      OutDocument->SetStringField(TEXT("skeleton"), BlendSpace->GetSkeleton() ? BlendSpace->GetSkeleton()->GetPathName() : FString());
      OutDocument->SetNumberField(TEXT("dimension"), BlendSpace->IsA<UBlendSpace1D>() ? 1 : 2);
    }
    break;
  }
  if (!OutDocument.IsValid() && OutError.IsEmpty())
    OutError = AssetTypeFailure(TEXT("animation_document_asset_type_mismatch"), Kind, Asset,
        TEXT("Animation document capture"), CanonicalAssetPath);
  return OutDocument.IsValid();
}

bool BuildDocument(const EAssetKind Kind, UObject &Asset,
                   TSharedPtr<FJsonObject> &OutDocument, UE::UAI::Contract::FDiagnostic& OutError,
                   const FString &CanonicalAssetPath) {
  return BuildDocumentForPurpose(Kind, Asset, OutDocument, OutError, CanonicalAssetPath, false);
}

bool BuildVerificationDocument(const EAssetKind Kind, UObject &Asset,
                   TSharedPtr<FJsonObject> &OutDocument, UE::UAI::Contract::FDiagnostic& OutError,
                   const FString &CanonicalAssetPath) {
  return BuildDocumentForPurpose(Kind, Asset, OutDocument, OutError, CanonicalAssetPath, true);
}

bool ApplyDocument(const EAssetKind Kind, UObject &Asset,
                   const TSharedPtr<const FJsonObject> &Document,
                   FMutationStats &OutStats, UE::UAI::Contract::FDiagnostic& OutError,
                   const FString &CanonicalAssetPath) {
  OutStats = FMutationStats();
  OutError.Reset();
  if (!Document.IsValid()) {
    OutError = DocumentInputFailure(TEXT("animation_document_missing"),
        TEXT("Applying an animation document requires a document object."), TEXT("/"), nullptr, TEXT("document object"));
    return false;
  }
  switch (Kind) {
  case EAssetKind::PoseAsset:
    if (auto* Pose = Cast<UPoseAsset>(&Asset)) return ApplyPoseStructure(*Pose, Document, OutError, CanonicalAssetPath);
    break;
  case EAssetKind::AnimBlueprint:
    if (UAnimBlueprint *Blueprint = Cast<UAnimBlueprint>(&Asset))
      return ApplyAnimBlueprintDocument(*Blueprint, Document, OutStats,
                                        OutError, CanonicalAssetPath);
    break;
  case EAssetKind::Skeleton:
    if (USkeleton *Skeleton = Cast<USkeleton>(&Asset))
      return ApplySkeletonDocument(*Skeleton, Document, OutStats, OutError,
          CanonicalAssetPath.IsEmpty() ? Skeleton->GetPathName() : ToObjectPath(CanonicalAssetPath));
    OutError = AssetTypeFailure(TEXT("animation_document_asset_type_mismatch"), Kind, Asset,
        TEXT("Skeleton document apply"), CanonicalAssetPath);
    OutError.Stage = TEXT("document_apply");
    break;
  case EAssetKind::SkeletalMesh:
    if (USkeletalMesh *Mesh = Cast<USkeletalMesh>(&Asset))
      return ApplySkeletalMeshDocument(*Mesh, Document, OutStats, OutError);
    break;
  case EAssetKind::AnimSequence:
    if (UAnimSequence* Sequence = Cast<UAnimSequence>(&Asset)) {
      if (Document->HasField(TEXT("metadata_changes")) || Document->HasField(TEXT("curve_changes")) ||
          Document->HasField(TEXT("event_changes"))) {
        using namespace UE::UAI::Authoring::Timeline;
        FSequenceChanges Changes;
        FUeAgentTimelineAnimationExecutionError Error;
        const FString ObjectPath = CanonicalAssetPath.IsEmpty() ? Sequence->GetPathName() : ToObjectPath(CanonicalAssetPath);
        if (!ParseSequenceChanges(ConstCastSharedPtr<FJsonObject>(Document), Changes, Error)) {
          OutError = Error;
          return false;
        }
        if (!ApplySequenceStructure(*Sequence, Document, OutStats.Settings, OutError, CanonicalAssetPath)) return false;
        if (!ApplySequenceMetadata(*Sequence, Changes, Error, ObjectPath) ||
            !ApplySequenceCurves(*Sequence, Changes, Error, ObjectPath) ||
            !ApplyAnimationEventChanges(*Sequence, Changes, Error, ObjectPath)) {
          OutError = Error;
          return false;
        }
        OutStats.Settings += Changes.MetadataChanges.Num() + Changes.CurveChanges.Num() + Changes.EventChanges.Num();
        return true;
      }
      return ApplySequenceStructure(*Sequence, Document, OutStats.Settings, OutError, CanonicalAssetPath);
    }
    break;
  case EAssetKind::AnimMontage:
    if (auto* Montage = Cast<UAnimMontage>(&Asset)) return ApplyMontageStructure(*Montage, Document, OutStats.Settings, OutError, CanonicalAssetPath);
    break;
  case EAssetKind::BlendSpace:
    if (auto* BlendSpace = Cast<UBlendSpace>(&Asset)) {
      const FJsonValueString Skeleton(BlendSpace->GetSkeleton() ? BlendSpace->GetSkeleton()->GetPathName() : FString());
      const FJsonValueNumber Dimension(BlendSpace->IsA<UBlendSpace1D>() ? 1 : 2);
      const bool bSkeletonChanged = Document->HasField(TEXT("skeleton")) && !FJsonValue::CompareEqual(*Document->TryGetField(TEXT("skeleton")), Skeleton);
      if (bSkeletonChanged ||
          (Document->HasField(TEXT("dimension")) && !FJsonValue::CompareEqual(*Document->TryGetField(TEXT("dimension")), Dimension))) {
        const TCHAR* Field = bSkeletonChanged ? TEXT("skeleton") : TEXT("dimension");
        OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("animation_blend_space_creation_context_changed"),
            TEXT("/") + FString(Field), bSkeletonChanged ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Skeleton.AsString()))
                : MakeShared<FJsonValueNumber>(Dimension.AsNumber()), Document->TryGetField(Field), TEXT("animation_authoring"),
            bSkeletonChanged ? TEXT("The supplied skeleton differs from the BlendSpace creation skeleton.")
                : TEXT("The supplied dimension differs from the BlendSpace creation dimension."));
        UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Document, Field);
        return false;
      }
      return true;
    }
    break;
  }
  if (OutError.IsEmpty())
    OutError = AssetTypeFailure(TEXT("animation_document_asset_type_mismatch"), Kind, Asset,
        TEXT("Animation document apply"), CanonicalAssetPath);
  return false;
}

bool CompileAsset(const EAssetKind Kind, UObject &Asset, UE::UAI::Contract::FDiagnostic& OutError) {
  OutError.Reset();
  if (Kind == EAssetKind::BlendSpace) {
    if (UBlendSpace* BlendSpace = Cast<UBlendSpace>(&Asset)) {
      BlendSpace->ValidateSampleData();
      BlendSpace->ResampleData();
      return true;
    }
    OutError = AssetTypeFailure(TEXT("animation_document_asset_type_mismatch"), Kind, Asset, TEXT("BlendSpace compilation"));
    return false;
  }
  if (Kind == EAssetKind::AnimSequence) {
    if (UAnimSequence* Sequence = Cast<UAnimSequence>(&Asset)) {
      Sequence->WaitOnExistingCompression();
      return true;
    }
    OutError = AssetTypeFailure(TEXT("animation_document_asset_type_mismatch"), Kind, Asset, TEXT("Animation sequence compilation"));
    return false;
  }
  if (Kind != EAssetKind::AnimBlueprint)
    return true;
  UAnimBlueprint *Blueprint = Cast<UAnimBlueprint>(&Asset);
  if (!Blueprint) {
    OutError = AssetTypeFailure(TEXT("animation_compile_asset_type_mismatch"), Kind, Asset, TEXT("Animation Blueprint compilation"));
    return false;
  }
  FCompilerResultsLog CompilerLog;
  FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &CompilerLog);
  if (CompilerLog.NumErrors > 0 || Blueprint->Status == BS_Error) {
    OutError = UeAgentAuthoringDiagnostics::MakeCompileFailure(
        TEXT("animation_anim_blueprint_compile_failed"),
        UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(*Blueprint), CompilerLog, Blueprint);
    if (CompilerLog.NumErrors == 0) {
      OutError.PublicMessage = TEXT("Animation Blueprint compilation ended in error status without a compiler error message.");
      OutError.ActualValue = MakeShared<FJsonValueString>(TEXT("BS_Error"));
      OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("compiled Blueprint without error status"));
    }
    return false;
  }
  return true;
}

bool SaveAsset(const EAssetKind Kind, UObject &Asset, UE::UAI::Contract::FDiagnostic& OutError) {
  return UeAgentAnimationAssetOps::SaveAssetPackage(&Asset, OutError);
}
} // namespace UeAgentAnimationAuthoring
