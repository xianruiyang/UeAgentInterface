// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringDocuments.h"

#include "AnimGraphNode_StateMachineBase.h"
#include "AnimStateNodeBase.h"
#include "AnimStateTransitionNode.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
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
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Utilities/UeAgentPropertyValueCodec.h"

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
                  void *OutValue, UObject *Owner, FString &OutError) {
  FCodecOptions Options;
  Options.bAllowPartialStructFields = true;
  return UeAgentPropertyValueCodec::DecodeStructValue(Struct, Value, OutValue,
                                                      Owner, Options, OutError);
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

bool TryGetObject(const TSharedPtr<const FJsonObject> &Owner,
                  const TCHAR *Field, TSharedPtr<const FJsonObject> &OutObject,
                  FString &OutError) {
  OutObject.Reset();
  if (!Owner.IsValid() || !Owner->HasField(Field))
    return true;
  const TSharedPtr<FJsonObject> *Object = nullptr;
  if (!Owner->TryGetObjectField(Field, Object) || !Object ||
      !Object->IsValid()) {
    OutError =
        FString::Printf(TEXT("animation_document_expected_object:%s"), Field);
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
                         FString &OutError) {
  if (!Requested.IsValid() || !Requested->HasField(Field))
    return true;
  const TSharedPtr<FJsonValue> RequestedValue = Requested->TryGetField(Field);
  const TSharedPtr<FJsonValue> CurrentValue =
      Current.IsValid() ? Current->TryGetField(Field) : nullptr;
  if (!RequestedValue.IsValid() || !CurrentValue.IsValid() ||
      !CanonicalEqual(RequestedValue, CurrentValue)) {
    OutError =
        FString::Printf(TEXT("%s_read_only_mismatch:%s"), *Prefix, Field);
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

bool ParseSocketObject(const TSharedPtr<FJsonObject> &Object, UObject &Owner,
                       const USkeletalMeshSocket *Existing,
                       FSocketSpec &OutSpec, FString &OutError) {
  if (!Object.IsValid() ||
      !ValidateFields(Object,
                      {TEXT("name"), TEXT("bone"), TEXT("location"),
                       TEXT("rotation"), TEXT("scale")},
                      TEXT("animation_socket"), OutError)) {
    return false;
  }
  FString Name;
  if (!Object->TryGetStringField(TEXT("name"), Name) ||
      Name.TrimStartAndEnd().IsEmpty()) {
    OutError = TEXT("animation_socket_name_missing");
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
    OutError = TEXT("animation_socket_bone_missing:") + OutSpec.Name.ToString();
    return false;
  }
  auto DecodeIfPresent = [&](const TCHAR *Field, UScriptStruct *Struct,
                             void *Value) -> bool {
    const TSharedPtr<FJsonValue> Json = Object->TryGetField(Field);
    if (!Json.IsValid())
      return true;
    FString Error;
    if (!DecodeStruct(Struct, Json, Value, &Owner, Error)) {
      OutError = FString::Printf(TEXT("animation_socket_%s_invalid:%s"), Field,
                                 *Error);
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

void ApplySocketSpec(USkeletalMeshSocket &Socket, const FSocketSpec &Spec) {
  Socket.SocketName = Spec.Name;
  Socket.BoneName = Spec.Bone;
  Socket.RelativeLocation = Spec.Location;
  Socket.RelativeRotation = Spec.Rotation;
  Socket.RelativeScale = Spec.Scale;
}

TSharedPtr<FJsonObject>
BuildAnimBlueprintDocument(UAnimBlueprint &Blueprint, FString &OutError,
                           const FString &CanonicalAssetPath) {
  TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
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
  Document->SetObjectField(TEXT("settings"), Settings);

  TSharedPtr<FJsonObject> BlueprintContext;
  if (!UeAgentBlueprintAuthoring::BuildReusableProviderExtension(
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
                                FMutationStats &OutStats, FString &OutError,
                                const FString &CanonicalAssetPath) {
  if (!ValidateFields(
          Document,
          {TEXT("settings"), TEXT("blueprint"), TEXT("state_machines")},
          TEXT("animation_anim_blueprint"), OutError))
    return false;
  TSharedPtr<FJsonObject> Current;
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
  auto BeginMutation = [&Blueprint, &bChanged]() {
    if (!bChanged)
      Blueprint.Modify();
    bChanged = true;
  };
  FString Path;
  if (Settings->TryGetStringField(TEXT("parent_class"), Path) &&
      !Path.Equals(ObjectPath(Blueprint.ParentClass),
                   ESearchCase::CaseSensitive)) {
    UClass *ParentClass = LoadClass(Path);
    if (!ParentClass || !ParentClass->IsChildOf(UAnimInstance::StaticClass()) ||
        !FKismetEditorUtilities::CanCreateBlueprintOfClass(ParentClass)) {
      OutError = TEXT("animation_anim_blueprint_parent_class_invalid:") + Path;
      return false;
    }
    BeginMutation();
    UBlueprintEditorLibrary::ReparentBlueprint(&Blueprint, ParentClass);
  }

  if (Settings->TryGetStringField(TEXT("preview_skeletal_mesh"), Path) &&
      !Path.Equals(ObjectPath(Blueprint.GetPreviewMesh()),
                   ESearchCase::CaseSensitive)) {
    USkeletalMesh *Mesh = LoadTypedAsset<USkeletalMesh>(Path);
    if (!Path.IsEmpty() && !Mesh) {
      OutError = TEXT("animation_preview_skeletal_mesh_not_found:") + Path;
      return false;
    }
    if (Mesh && Blueprint.TargetSkeleton && Mesh->GetSkeleton() &&
        !Blueprint.TargetSkeleton->IsCompatibleForEditor(Mesh->GetSkeleton())) {
      OutError = TEXT("animation_preview_skeletal_mesh_incompatible:") + Path;
      return false;
    }
    BeginMutation();
    Blueprint.SetPreviewMesh(Mesh, false);
  }

  if (Settings->TryGetStringField(TEXT("preview_animation_blueprint"), Path) &&
      !Path.Equals(ObjectPath(Blueprint.GetPreviewAnimationBlueprint()),
                   ESearchCase::CaseSensitive)) {
    UAnimBlueprint *Preview = LoadTypedAsset<UAnimBlueprint>(Path);
    if (!Path.IsEmpty() && !Preview) {
      OutError = TEXT("animation_preview_anim_blueprint_not_found:") + Path;
      return false;
    }
    if (Preview == &Blueprint ||
        (Preview && !Blueprint.IsCompatible(Preview))) {
      OutError = TEXT("animation_preview_anim_blueprint_incompatible:") + Path;
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
      OutError =
          TEXT("animation_preview_application_method_invalid:") + MethodText;
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
                          FMutationStats &OutStats, FString &OutError) {
  TMap<FName, const USkeletalMeshSocket *> ExistingByName;
  for (const USkeletalMeshSocket *Socket : Skeleton.Sockets)
    if (Socket)
      ExistingByName.Add(Socket->SocketName, Socket);
  TArray<FSocketSpec> Specs;
  TSet<FName> Names;
  for (const TSharedPtr<FJsonValue> &Value : Values) {
    if (!Value.IsValid() || Value->Type != EJson::Object) {
      OutError = TEXT("animation_skeleton_socket_expected_object");
      return false;
    }
    TSharedPtr<FJsonObject> Object = Value->AsObject();
    FString Name;
    Object->TryGetStringField(TEXT("name"), Name);
    FSocketSpec Spec;
    if (!ParseSocketObject(Object, Skeleton,
                           ExistingByName.FindRef(FName(*Name)), Spec,
                           OutError))
      return false;
    if (Names.Contains(Spec.Name)) {
      OutError =
          TEXT("animation_skeleton_socket_duplicate:") + Spec.Name.ToString();
      return false;
    }
    if (Skeleton.GetReferenceSkeleton().FindBoneIndex(Spec.Bone) ==
        INDEX_NONE) {
      OutError = TEXT("animation_skeleton_socket_bone_not_found:") +
                 Spec.Bone.ToString();
      return false;
    }
    Names.Add(Spec.Name);
    Specs.Add(Spec);
  }
  Specs.Sort([](const FSocketSpec &A, const FSocketSpec &B) {
    return A.Name.LexicalLess(B.Name);
  });
  Skeleton.Modify();
  Skeleton.Sockets.Reset();
  for (const FSocketSpec &Spec : Specs) {
    USkeletalMeshSocket *Socket =
        NewObject<USkeletalMeshSocket>(&Skeleton, NAME_None, RF_Transactional);
    ApplySocketSpec(*Socket, Spec);
    Skeleton.Sockets.Add(Socket);
  }
  Skeleton.MarkPackageDirty();
  OutStats.Sockets += Specs.Num();
  return true;
}

bool ApplySkeletonDocument(USkeleton &Skeleton,
                           const TSharedPtr<const FJsonObject> &Document,
                           FMutationStats &OutStats, FString &OutError) {
  if (!ValidateFields(Document,
                      {TEXT("settings"), TEXT("sockets"), TEXT("virtual_bones"),
                       TEXT("slot_groups"), TEXT("retargeting"), TEXT("bones")},
                      TEXT("animation_skeleton"), OutError))
    return false;
  TSharedPtr<FJsonObject> Current;
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
                        TEXT("animation_skeleton_settings"), OutError))
      return false;
    FString PreviewPath;
    if (Settings->TryGetStringField(TEXT("preview_skeletal_mesh"),
                                    PreviewPath) &&
        !PreviewPath.Equals(ObjectPath(Skeleton.GetPreviewMesh()),
                            ESearchCase::CaseSensitive)) {
      USkeletalMesh *Preview = LoadTypedAsset<USkeletalMesh>(PreviewPath);
      if (!PreviewPath.IsEmpty() && !Preview) {
        OutError =
            TEXT("animation_skeleton_preview_mesh_not_found:") + PreviewPath;
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
        for (const TSharedPtr<FJsonValue> &Value : *CompatibleValues) {
          if (!Value.IsValid() || Value->Type != EJson::String) {
            OutError = TEXT("animation_compatible_skeleton_path_expected");
            return false;
          }
          const FString Path = Value->AsString();
          USkeleton *Compatible = LoadTypedAsset<USkeleton>(Path);
          if (!Compatible || Compatible == &Skeleton || Paths.Contains(Path)) {
            OutError = TEXT("animation_compatible_skeleton_invalid:") + Path;
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
    return Apply(*Values);
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
            for (const TSharedPtr<FJsonValue> &Value : Values) {
              if (!Value.IsValid() || Value->Type != EJson::Object) {
                OutError = TEXT("animation_virtual_bone_expected_object");
                return false;
              }
              TSharedPtr<FJsonObject> Item = Value->AsObject();
              if (!ValidateFields(
                      Item, {TEXT("name"), TEXT("source"), TEXT("target")},
                      TEXT("animation_virtual_bone"), OutError))
                return false;
              FString Name, Source, Target;
              Item->TryGetStringField(TEXT("name"), Name);
              Item->TryGetStringField(TEXT("source"), Source);
              Item->TryGetStringField(TEXT("target"), Target);
              FVirtualBoneSpec Spec{FName(*Name), FName(*Source),
                                    FName(*Target)};
              if (Spec.Source.IsNone() || Spec.Target.IsNone() ||
                  Skeleton.GetReferenceSkeleton().FindBoneIndex(Spec.Source) ==
                      INDEX_NONE ||
                  Skeleton.GetReferenceSkeleton().FindBoneIndex(Spec.Target) ==
                      INDEX_NONE ||
                  (!Spec.Name.IsNone() && Names.Contains(Spec.Name))) {
                OutError = TEXT("animation_virtual_bone_invalid:") + Name;
                return false;
              }
              if (!Spec.Name.IsNone())
                Names.Add(Spec.Name);
              Specs.Add(Spec);
            }
            Skeleton.Modify();
            TArray<FName> RemoveNames;
            for (const FVirtualBone &Bone : Skeleton.GetVirtualBones())
              RemoveNames.Add(Bone.VirtualBoneName);
            if (!RemoveNames.IsEmpty())
              Skeleton.RemoveVirtualBones(RemoveNames);
            for (const FVirtualBoneSpec &Spec : Specs) {
              FName ResultName = Spec.Name;
              const bool bAdded =
                  Spec.Name.IsNone()
                      ? Skeleton.AddNewVirtualBone(Spec.Source, Spec.Target,
                                                   ResultName)
                      : Skeleton.AddNewNamedVirtualBone(
                            Spec.Source, Spec.Target, ResultName);
              if (!bAdded) {
                OutError = TEXT("animation_virtual_bone_add_failed:") +
                           Spec.Name.ToString();
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
            for (const TSharedPtr<FJsonValue> &Value : Values) {
              if (!Value.IsValid() || Value->Type != EJson::Object) {
                OutError = TEXT("animation_slot_group_expected_object");
                return false;
              }
              TSharedPtr<FJsonObject> Item = Value->AsObject();
              if (!ValidateFields(Item, {TEXT("name"), TEXT("slots")},
                                  TEXT("animation_slot_group"), OutError))
                return false;
              FString GroupText;
              const TArray<TSharedPtr<FJsonValue>> *Slots = nullptr;
              if (!Item->TryGetStringField(TEXT("name"), GroupText) ||
                  GroupText.IsEmpty() ||
                  !Item->TryGetArrayField(TEXT("slots"), Slots) || !Slots ||
                  GroupNames.Contains(FName(*GroupText))) {
                OutError = TEXT("animation_slot_group_invalid:") + GroupText;
                return false;
              }
              FGroup Group;
              Group.Name = FName(*GroupText);
              GroupNames.Add(Group.Name);
              for (const TSharedPtr<FJsonValue> &SlotValue : *Slots) {
                if (!SlotValue.IsValid() || SlotValue->Type != EJson::String) {
                  OutError = TEXT("animation_slot_name_expected");
                  return false;
                }
                const FName Slot(*SlotValue->AsString());
                if (Slot.IsNone() || SlotNames.Contains(Slot)) {
                  OutError =
                      TEXT("animation_slot_name_duplicate:") + Slot.ToString();
                  return false;
                }
                SlotNames.Add(Slot);
                Group.Slots.Add(Slot);
              }
              Groups.Add(MoveTemp(Group));
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
                Skeleton.RegisterSlotNode(Slot);
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
            for (const TSharedPtr<FJsonValue> &Value : Values) {
              if (!Value.IsValid() || Value->Type != EJson::Object) {
                OutError = TEXT("animation_retargeting_expected_object");
                return false;
              }
              TSharedPtr<FJsonObject> Item = Value->AsObject();
              if (!ValidateFields(Item, {TEXT("bone"), TEXT("mode")},
                                  TEXT("animation_retargeting"), OutError))
                return false;
              FString BoneText, ModeText;
              Item->TryGetStringField(TEXT("bone"), BoneText);
              Item->TryGetStringField(TEXT("mode"), ModeText);
              const FName Bone(*BoneText);
              const int32 Index =
                  Skeleton.GetReferenceSkeleton().FindBoneIndex(Bone);
              EBoneTranslationRetargetingMode::Type Mode;
              if (Index == INDEX_NONE || Bones.Contains(Bone) ||
                  !ParseRetargetMode(ModeText, Mode)) {
                OutError = TEXT("animation_retargeting_invalid:") + BoneText;
                return false;
              }
              Bones.Add(Bone);
              Modes.Emplace(Index, Mode);
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
                              FMutationStats &OutStats, FString &OutError) {
  TMap<FName, const USkeletalMeshSocket *> ExistingByName;
  for (const USkeletalMeshSocket *Socket : Mesh.GetMeshOnlySocketList())
    if (Socket)
      ExistingByName.Add(Socket->SocketName, Socket);
  TArray<FSocketSpec> Specs;
  TSet<FName> Names;
  for (const TSharedPtr<FJsonValue> &Value : Values) {
    if (!Value.IsValid() || Value->Type != EJson::Object) {
      OutError = TEXT("animation_mesh_socket_expected_object");
      return false;
    }
    TSharedPtr<FJsonObject> Object = Value->AsObject();
    FString Name;
    Object->TryGetStringField(TEXT("name"), Name);
    FSocketSpec Spec;
    if (!ParseSocketObject(Object, Mesh, ExistingByName.FindRef(FName(*Name)),
                           Spec, OutError))
      return false;
    if (Names.Contains(Spec.Name) ||
        Mesh.GetRefSkeleton().FindBoneIndex(Spec.Bone) == INDEX_NONE) {
      OutError = TEXT("animation_mesh_socket_invalid:") + Spec.Name.ToString();
      return false;
    }
    Names.Add(Spec.Name);
    Specs.Add(Spec);
  }
  Specs.Sort([](const FSocketSpec &A, const FSocketSpec &B) {
    return A.Name.LexicalLess(B.Name);
  });
  Mesh.Modify();
  Mesh.GetMeshOnlySocketList().Reset();
  for (const FSocketSpec &Spec : Specs) {
    USkeletalMeshSocket *Socket =
        NewObject<USkeletalMeshSocket>(&Mesh, NAME_None, RF_Transactional);
    ApplySocketSpec(*Socket, Spec);
    Mesh.AddSocket(Socket, false);
  }
  Mesh.MarkPackageDirty();
  OutStats.Sockets += Specs.Num();
  return true;
}

bool ApplySkeletalMeshDocument(USkeletalMesh &Mesh,
                               const TSharedPtr<const FJsonObject> &Document,
                               FMutationStats &OutStats, FString &OutError) {
  if (!ValidateFields(Document,
                      {TEXT("settings"), TEXT("materials"), TEXT("sockets"),
                       TEXT("summary"), TEXT("lods"), TEXT("sections"),
                       TEXT("morph_targets"), TEXT("import_data")},
                      TEXT("animation_skeletal_mesh"), OutError))
    return false;
  TSharedPtr<FJsonObject> Current;
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
        OutError = TEXT("animation_physics_asset_not_found:") + Path;
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
        OutError =
            TEXT("animation_post_process_anim_blueprint_invalid:") + Path;
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
        OutError = TEXT("animation_material_expected_object");
        return false;
      }
      TSharedPtr<FJsonObject> Item = Value->AsObject();
      if (!ValidateFields(Item,
                          {TEXT("slot_index"), TEXT("slot_name"),
                           TEXT("material"), TEXT("imported_slot_name")},
                          TEXT("animation_material"), OutError))
        return false;
      double IndexNumber = InputIndex;
      Item->TryGetNumberField(TEXT("slot_index"), IndexNumber);
      const int32 Index = FMath::RoundToInt(IndexNumber);
      if (Index < 0 || Indices.Contains(Index)) {
        OutError = TEXT("animation_material_slot_index_invalid");
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
          OutError = TEXT("animation_material_not_found:") + Text;
          return false;
        }
        Materials[Index].MaterialInterface = Material;
      }
    }
    for (int32 Index = 0; Index < Materials.Num(); ++Index)
      if (!Indices.Contains(Index)) {
        OutError = TEXT("animation_material_slot_index_gap");
        return false;
      }
    Mesh.Modify();
    Mesh.SetMaterials(Materials);
    Mesh.MarkPackageDirty();
    OutStats.Materials += Materials.Num();
  }

  const TArray<TSharedPtr<FJsonValue>> *SocketValues = nullptr;
  if (Document->TryGetArrayField(TEXT("sockets"), SocketValues) &&
      SocketValues &&
      !CanonicalEqual(Document->TryGetField(TEXT("sockets")),
                      Current->TryGetField(TEXT("sockets")))) {
    if (!ApplySkeletalMeshSockets(Mesh, *SocketValues, OutStats, OutError))
      return false;
  }
  return true;
}
} // namespace

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
  default:
    return nullptr;
  }
}

UObject *CreateFactorySeed(const EAssetKind Kind, UObject *Outer,
                           const FName Name, const EObjectFlags Flags,
                           const TSharedPtr<const FJsonObject> &SeedDocument,
                           FString &OutError) {
  OutError.Reset();
  if (!Outer) {
    OutError = TEXT("animation_factory_seed_outer_missing");
    return nullptr;
  }
  if (Kind == EAssetKind::Skeleton)
    return NewObject<USkeleton>(Outer, Name, Flags);
  if (Kind == EAssetKind::SkeletalMesh) {
    USkeletalMesh *Mesh = NewObject<USkeletalMesh>(Outer, Name, Flags);
    Mesh->AllocateResourceForRendering();
    TSharedPtr<const FJsonObject> Settings;
    if (!TryGetObject(SeedDocument, TEXT("settings"), Settings, OutError))
      return nullptr;
    FString SkeletonPath;
    if (Settings.IsValid() &&
        Settings->TryGetStringField(TEXT("skeleton"), SkeletonPath)) {
      USkeleton *Skeleton = LoadTypedAsset<USkeleton>(SkeletonPath);
      if (!SkeletonPath.IsEmpty() && !Skeleton) {
        OutError = TEXT("animation_skeletal_mesh_seed_skeleton_not_found:") +
                   SkeletonPath;
        return nullptr;
      }
      Mesh->SetSkeleton(Skeleton);
    }
    return Mesh;
  }
  if (Kind != EAssetKind::AnimBlueprint) {
    OutError = TEXT("animation_factory_seed_kind_unsupported");
    return nullptr;
  }

  UAnimBlueprintFactory *Factory = NewObject<UAnimBlueprintFactory>();
  if (!Factory) {
    OutError = TEXT("animation_anim_blueprint_factory_create_failed");
    return nullptr;
  }
  TSharedPtr<const FJsonObject> Settings;
  if (!TryGetObject(SeedDocument, TEXT("settings"), Settings, OutError))
    return nullptr;
  if (Settings.IsValid()) {
    FString Path;
    if (Settings->TryGetStringField(TEXT("parent_class"), Path)) {
      Factory->ParentClass = LoadClass(Path);
      if (!Factory->ParentClass ||
          !Factory->ParentClass->IsChildOf(UAnimInstance::StaticClass()) ||
          !FKismetEditorUtilities::CanCreateBlueprintOfClass(
              Factory->ParentClass)) {
        OutError = TEXT("animation_anim_blueprint_seed_parent_invalid:") + Path;
        return nullptr;
      }
    }
    Settings->TryGetBoolField(TEXT("template"), Factory->bTemplate);
    if (Settings->TryGetStringField(TEXT("preview_skeletal_mesh"), Path)) {
      Factory->PreviewSkeletalMesh = LoadTypedAsset<USkeletalMesh>(Path);
      if (!Path.IsEmpty() && !Factory->PreviewSkeletalMesh) {
        OutError =
            TEXT("animation_anim_blueprint_seed_preview_mesh_not_found:") +
            Path;
        return nullptr;
      }
    }
    if (Settings->TryGetStringField(TEXT("target_skeleton"), Path)) {
      Factory->TargetSkeleton = LoadTypedAsset<USkeleton>(Path);
      if (!Path.IsEmpty() && !Factory->TargetSkeleton) {
        OutError =
            TEXT("animation_anim_blueprint_seed_skeleton_not_found:") + Path;
        return nullptr;
      }
    }
    if (!Factory->TargetSkeleton && Factory->PreviewSkeletalMesh)
      Factory->TargetSkeleton = Factory->PreviewSkeletalMesh->GetSkeleton();
    if (Factory->bTemplate &&
        (Factory->TargetSkeleton || Factory->PreviewSkeletalMesh)) {
      OutError = TEXT("animation_anim_blueprint_template_seed_has_skeleton");
      return nullptr;
    }
  }
  Factory->BlueprintType = BPTYPE_Normal;
  return Factory->FactoryCreateNew(UAnimBlueprint::StaticClass(), Outer, Name,
                                   Flags, nullptr, GWarn,
                                   FName(TEXT("UeAgentInterface")));
}

bool BuildDocument(const EAssetKind Kind, UObject &Asset,
                   TSharedPtr<FJsonObject> &OutDocument, FString &OutError,
                   const FString &CanonicalAssetPath) {
  OutDocument.Reset();
  OutError.Reset();
  switch (Kind) {
  case EAssetKind::AnimBlueprint:
    if (UAnimBlueprint *Blueprint = Cast<UAnimBlueprint>(&Asset))
      OutDocument =
          BuildAnimBlueprintDocument(*Blueprint, OutError, CanonicalAssetPath);
    break;
  case EAssetKind::Skeleton:
    if (USkeleton *Skeleton = Cast<USkeleton>(&Asset))
      OutDocument = BuildSkeletonDocument(*Skeleton);
    break;
  case EAssetKind::SkeletalMesh:
    if (USkeletalMesh *Mesh = Cast<USkeletalMesh>(&Asset))
      OutDocument = BuildSkeletalMeshDocument(*Mesh);
    break;
  }
  if (!OutDocument.IsValid() && OutError.IsEmpty())
    OutError = TEXT("animation_document_asset_type_mismatch");
  return OutDocument.IsValid();
}

bool ApplyDocument(const EAssetKind Kind, UObject &Asset,
                   const TSharedPtr<const FJsonObject> &Document,
                   FMutationStats &OutStats, FString &OutError,
                   const FString &CanonicalAssetPath) {
  OutStats = FMutationStats();
  OutError.Reset();
  if (!Document.IsValid()) {
    OutError = TEXT("animation_document_missing");
    return false;
  }
  switch (Kind) {
  case EAssetKind::AnimBlueprint:
    if (UAnimBlueprint *Blueprint = Cast<UAnimBlueprint>(&Asset))
      return ApplyAnimBlueprintDocument(*Blueprint, Document, OutStats,
                                        OutError, CanonicalAssetPath);
    break;
  case EAssetKind::Skeleton:
    if (USkeleton *Skeleton = Cast<USkeleton>(&Asset))
      return ApplySkeletonDocument(*Skeleton, Document, OutStats, OutError);
    break;
  case EAssetKind::SkeletalMesh:
    if (USkeletalMesh *Mesh = Cast<USkeletalMesh>(&Asset))
      return ApplySkeletalMeshDocument(*Mesh, Document, OutStats, OutError);
    break;
  }
  OutError = TEXT("animation_document_asset_type_mismatch");
  return false;
}

bool CompileAsset(const EAssetKind Kind, UObject &Asset, FString &OutError) {
  OutError.Reset();
  if (Kind != EAssetKind::AnimBlueprint)
    return true;
  UAnimBlueprint *Blueprint = Cast<UAnimBlueprint>(&Asset);
  if (!Blueprint) {
    OutError = TEXT("animation_compile_asset_type_mismatch");
    return false;
  }
  FKismetEditorUtilities::CompileBlueprint(Blueprint);
  if (Blueprint->Status == BS_Error) {
    OutError = TEXT("animation_anim_blueprint_compile_failed");
    return false;
  }
  return true;
}

bool SaveAsset(const EAssetKind Kind, UObject &Asset, FString &OutError) {
  return UeAgentAnimationAssetOps::SaveAssetPackage(&Asset, OutError);
}
} // namespace UeAgentAnimationAuthoring
