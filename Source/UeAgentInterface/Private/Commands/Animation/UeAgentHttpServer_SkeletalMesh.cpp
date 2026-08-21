// Copyright Epic Games, Inc. All Rights Reserved.

#include "LegacyMechanics/UeAgentLegacyMechanics.h"

#include "Commands/Animation/UeAgentAnimationFolderCommandBridge.h"
#include "Utilities/UeAgentAuthoringJson.h"
#include "Utilities/UeAgentJsonDiagnostics.h"

#include "Animation/AnimInstance.h"
#include "Animation/MeshDeformer.h"
#include "Animation/MeshDeformerCollection.h"
#include "Animation/MorphTarget.h"
#include "Animation/Skeleton.h"
#include "Animation/SkinWeightProfile.h"
#include "Components/SkeletalMeshComponent.h"
#include "EditorFramework/AssetImportData.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "FileHelpers.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshModel.h"
#include "SkinWeightsUtilities.h"

namespace UeAgentSkeletalMeshOps {
static FString NormalizeAssetPath(const FString &InPath) {
  FString OutPath = InPath;
  OutPath.TrimStartAndEndInline();

  int32 DotIndex = INDEX_NONE;
  if (OutPath.FindChar(TEXT('.'), DotIndex) && DotIndex > 0) {
    OutPath = OutPath.Left(DotIndex);
  }
  return OutPath;
}

static FString ToObjectPath(const FString &InPath) {
  FString InObjectPath = InPath;
  InObjectPath.TrimStartAndEndInline();
  if (InObjectPath.IsEmpty()) {
    return FString();
  }
  if (InObjectPath.Contains(TEXT("."))) {
    return InObjectPath;
  }

  const FString AssetPath = NormalizeAssetPath(InObjectPath);
  const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
  return AssetName.IsEmpty()
             ? FString()
             : FString::Printf(TEXT("%s.%s"), *AssetPath, *AssetName);
}

static UObject *LoadAssetObject(const FString &InPath) {
  const FString ObjectPath = ToObjectPath(InPath);
  if (ObjectPath.IsEmpty()) {
    return nullptr;
  }
  if (UObject *Existing = FindObject<UObject>(nullptr, *ObjectPath)) {
    return Existing;
  }
  return LoadObject<UObject>(nullptr, *ObjectPath);
}

static USkeletalMesh *LoadSkeletalMesh(const FString &InPath) {
  return Cast<USkeletalMesh>(LoadAssetObject(InPath));
}
static bool SaveAssetPackage(UObject *Asset, FString &OutError) {
  if (!Asset) {
    OutError = TEXT("asset_not_found");
    return false;
  }
  TArray<UPackage *> PackagesToSave;
  PackagesToSave.Add(Asset->GetOutermost());
  if (!UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, false)) {
    OutError = TEXT("save_asset_failed");
    return false;
  }
  return true;
}

static TSharedPtr<FJsonObject> BuildVectorJson(const FVector &Value) {
  TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
  Obj->SetNumberField(TEXT("x"), Value.X);
  Obj->SetNumberField(TEXT("y"), Value.Y);
  Obj->SetNumberField(TEXT("z"), Value.Z);
  return Obj;
}

static TSharedPtr<FJsonObject> BuildRotatorJson(const FRotator &Value) {
  TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
  Obj->SetNumberField(TEXT("pitch"), Value.Pitch);
  Obj->SetNumberField(TEXT("yaw"), Value.Yaw);
  Obj->SetNumberField(TEXT("roll"), Value.Roll);
  return Obj;
}

static TSharedPtr<FJsonObject> BuildBoundsJson(const FBoxSphereBounds &Bounds) {
  TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
  Obj->SetObjectField(TEXT("origin"), BuildVectorJson(Bounds.Origin));
  Obj->SetObjectField(TEXT("box_extent"), BuildVectorJson(Bounds.BoxExtent));
  Obj->SetNumberField(TEXT("sphere_radius"), Bounds.SphereRadius);
  return Obj;
}

static TSharedPtr<FJsonObject>
BuildSocketJson(const USkeletalMeshSocket *Socket) {
  TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
  Obj->SetStringField(TEXT("name"),
                      Socket ? Socket->SocketName.ToString() : TEXT(""));
  Obj->SetStringField(TEXT("socket_name"),
                      Socket ? Socket->SocketName.ToString() : TEXT(""));
  Obj->SetStringField(TEXT("bone_name"),
                      Socket ? Socket->BoneName.ToString() : TEXT(""));
  Obj->SetObjectField(
      TEXT("relative_location"),
      BuildVectorJson(Socket ? Socket->RelativeLocation : FVector::ZeroVector));
  Obj->SetObjectField(TEXT("relative_rotation"),
                      BuildRotatorJson(Socket ? Socket->RelativeRotation
                                              : FRotator::ZeroRotator));
  Obj->SetObjectField(
      TEXT("relative_scale"),
      BuildVectorJson(Socket ? Socket->RelativeScale : FVector::OneVector));
  const FString SocketName = Socket ? Socket->SocketName.ToString() : TEXT("");
  UeAgentAuthoringJson::ApplyAuthoringMetadata(
      Obj, TEXT("socket_name"), SocketName, SocketName,
      TEXT("Skeletal Mesh|Sockets"), TEXT("skeletal_mesh_socket"));
  return Obj;
}
static TSharedPtr<FJsonObject> BuildMeshJson(USkeletalMesh *Mesh) {
  TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
  Obj->SetStringField(TEXT("asset_class"), TEXT("/Script/Engine.SkeletalMesh"));
  Obj->SetStringField(TEXT("skeleton"), Mesh && Mesh->GetSkeleton()
                                            ? Mesh->GetSkeleton()->GetPathName()
                                            : TEXT(""));
  Obj->SetStringField(TEXT("physics_asset"),
                      Mesh && Mesh->GetPhysicsAsset()
                          ? Mesh->GetPhysicsAsset()->GetPathName()
                          : TEXT(""));
  Obj->SetStringField(TEXT("post_process_anim_blueprint"),
                      Mesh && Mesh->GetPostProcessAnimBlueprint()
                          ? Mesh->GetPostProcessAnimBlueprint()->GetPathName()
                          : TEXT(""));
  Obj->SetNumberField(TEXT("lod_count"), Mesh ? Mesh->GetLODNum() : 0);
  Obj->SetNumberField(TEXT("material_slot_count"),
                      Mesh ? Mesh->GetMaterials().Num() : 0);
  Obj->SetNumberField(TEXT("socket_count"),
                      Mesh ? Mesh->GetMeshOnlySocketList().Num() : 0);
  Obj->SetNumberField(TEXT("morph_target_count"),
                      Mesh ? Mesh->GetMorphTargets().Num() : 0);
  Obj->SetNumberField(TEXT("skin_weight_profile_count"),
                      Mesh ? Mesh->GetNumSkinWeightProfiles() : 0);
  return Obj;
}

static TSharedPtr<FJsonObject> BuildMaterialsJson(USkeletalMesh *Mesh) {
  TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
  TArray<TSharedPtr<FJsonValue>> Materials;
  const TArray<FSkeletalMaterial> &MeshMaterials = Mesh->GetMaterials();
  for (int32 Index = 0; Index < MeshMaterials.Num(); ++Index) {
    const FSkeletalMaterial &Material = MeshMaterials[Index];
    TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
    Item->SetNumberField(TEXT("slot_index"), Index);
    Item->SetStringField(TEXT("slot_name"),
                         Material.MaterialSlotName.ToString());
    Item->SetStringField(TEXT("material"),
                         Material.MaterialInterface
                             ? Material.MaterialInterface->GetPathName()
                             : TEXT(""));
    Item->SetStringField(TEXT("imported_material_slot_name"),
                         Material.ImportedMaterialSlotName.ToString());
    UeAgentAuthoringJson::ApplyAuthoringMetadata(
        Item, TEXT("slot_index"), FString::FromInt(Index),
        Material.MaterialSlotName.ToString(), TEXT("Skeletal Mesh|Materials"),
        TEXT("material_slot"));
    Materials.Add(MakeShared<FJsonValueObject>(Item));
  }
  Obj->SetArrayField(TEXT("materials"), Materials);
  return Obj;
}

static TSharedPtr<FJsonObject> BuildLodsIndexJson(USkeletalMesh *Mesh) {
  TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
  TArray<TSharedPtr<FJsonValue>> Lods;
  const int32 LodCount = Mesh->GetLODNum();
  const FSkeletalMeshModel *ImportedModel = Mesh->GetImportedModel();
  for (int32 LodIndex = 0; LodIndex < LodCount; ++LodIndex) {
    TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
    Item->SetNumberField(TEXT("lod_index"), LodIndex);
    Item->SetStringField(TEXT("editable_mode"),
                         LodIndex == 0 ? TEXT("import_only")
                                       : TEXT("import_or_rebuild"));
    Item->SetStringField(TEXT("edit_policy"), TEXT("summary_only"));
    if (const FSkeletalMeshLODInfo *LodInfo = Mesh->GetLODInfo(LodIndex)) {
      Item->SetNumberField(TEXT("screen_size"), LodInfo->ScreenSize.Default);
    }
    if (ImportedModel && ImportedModel->LODModels.IsValidIndex(LodIndex)) {
      const FSkeletalMeshLODModel &LODModel =
          ImportedModel->LODModels[LodIndex];
      Item->SetNumberField(TEXT("vertex_count"), LODModel.NumVertices);
      Item->SetNumberField(TEXT("index_count"), LODModel.IndexBuffer.Num());
      Item->SetNumberField(TEXT("triangle_count"),
                           LODModel.IndexBuffer.Num() / 3);
      Item->SetNumberField(TEXT("section_count"), LODModel.Sections.Num());
      Item->SetNumberField(TEXT("active_bone_count"),
                           LODModel.ActiveBoneIndices.Num());
      Item->SetNumberField(TEXT("required_bone_count"),
                           LODModel.RequiredBones.Num());
      Item->SetBoolField(TEXT("has_skin_weight_profiles"),
                         LODModel.SkinWeightProfiles.Num() > 0);
    }
    bool bHasMorphTargets = false;
    for (const TObjectPtr<UMorphTarget> &MorphTarget :
         Mesh->GetMorphTargets()) {
      if (MorphTarget && MorphTarget->HasDataForLOD(LodIndex)) {
        bHasMorphTargets = true;
        break;
      }
    }
    Item->SetBoolField(TEXT("has_morph_targets"), bHasMorphTargets);
    Lods.Add(MakeShared<FJsonValueObject>(Item));
  }
  Obj->SetArrayField(TEXT("lods"), Lods);
  return Obj;
}
static TSharedPtr<FJsonObject>
BuildSkinWeightProfilesJson(USkeletalMesh *Mesh) {
  TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
  TArray<TSharedPtr<FJsonValue>> Profiles;
  if (Mesh) {
    const TArray<FSkinWeightProfileInfo> &ProfileInfos =
        Mesh->GetSkinWeightProfiles();
    for (const FSkinWeightProfileInfo &ProfileInfo : ProfileInfos) {
      TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
      Item->SetStringField(TEXT("profile_name"), ProfileInfo.Name.ToString());
      Item->SetBoolField(TEXT("is_available"), true);
      UeAgentAuthoringJson::ApplyAuthoringMetadata(
          Item, TEXT("profile_name"), ProfileInfo.Name.ToString(),
          ProfileInfo.Name.ToString(),
          TEXT("Skeletal Mesh|Skin Weight Profiles"),
          TEXT("skin_weight_profile"));
      TArray<TSharedPtr<FJsonValue>> LodSources;
#if WITH_EDITORONLY_DATA
      for (const TPair<int32, FString> &Pair : ProfileInfo.PerLODSourceFiles) {
        TSharedPtr<FJsonObject> Source = MakeShared<FJsonObject>();
        Source->SetNumberField(TEXT("lod_index"), Pair.Key);
        Source->SetStringField(TEXT("source_file"), Pair.Value);
        LodSources.Add(MakeShared<FJsonValueObject>(Source));
      }
#endif
      Item->SetArrayField(TEXT("lod_sources"), LodSources);
      Profiles.Add(MakeShared<FJsonValueObject>(Item));
    }
  }
  Obj->SetArrayField(TEXT("profiles"), Profiles);
  Obj->SetStringField(
      TEXT("edit_policy"),
      TEXT("remove_only; import_profile_requires_explicit_action_command"));
  return Obj;
}

static TSharedPtr<FJsonObject> BuildMorphTargetsJson(USkeletalMesh *Mesh) {
  TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
  Obj->SetStringField(
      TEXT("schema"),
      TEXT("ue_agent_interface.skeletal_mesh.morph_targets.v1"));
  TArray<TSharedPtr<FJsonValue>> MorphTargets;
  if (Mesh) {
    for (const TObjectPtr<UMorphTarget> &MorphTarget :
         Mesh->GetMorphTargets()) {
      if (!MorphTarget) {
        continue;
      }
      TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
      Item->SetStringField(TEXT("name"), MorphTarget->GetName());
      Item->SetBoolField(TEXT("has_valid_data"), MorphTarget->HasValidData());
      TArray<TSharedPtr<FJsonValue>> Lods;
      const TArray<FMorphTargetLODModel> &MorphLodModels =
          MorphTarget->GetMorphLODModels();
      for (int32 LodIndex = 0; LodIndex < MorphLodModels.Num(); ++LodIndex) {
        const FMorphTargetLODModel &MorphLod = MorphLodModels[LodIndex];
        TSharedPtr<FJsonObject> LodObj = MakeShared<FJsonObject>();
        LodObj->SetNumberField(TEXT("lod_index"), LodIndex);
        LodObj->SetNumberField(TEXT("delta_count"),
                               MorphLod.Vertices.Num() > 0
                                   ? MorphLod.Vertices.Num()
                                   : MorphLod.NumVertices);
        LodObj->SetNumberField(TEXT("affected_vertex_count"),
                               MorphLod.NumVertices);
        LodObj->SetNumberField(TEXT("num_base_mesh_vertices"),
                               MorphLod.NumBaseMeshVerts);
        LodObj->SetBoolField(TEXT("generated_by_engine"),
                             MorphLod.bGeneratedByEngine);
        LodObj->SetStringField(TEXT("source_import_file"),
                               MorphLod.SourceFilename);
        TArray<TSharedPtr<FJsonValue>> Sections;
        for (int32 SectionIndex : MorphLod.SectionIndices) {
          Sections.Add(MakeShared<FJsonValueNumber>(SectionIndex));
        }
        LodObj->SetArrayField(TEXT("section_coverage"), Sections);
        float MaxPositionDelta = 0.0f;
        float MaxTangentDelta = 0.0f;
        for (const FMorphTargetDelta &Delta : MorphLod.Vertices) {
          MaxPositionDelta =
              FMath::Max(MaxPositionDelta, Delta.PositionDelta.Length());
          MaxTangentDelta =
              FMath::Max(MaxTangentDelta, Delta.TangentZDelta.Length());
        }
        LodObj->SetNumberField(TEXT("max_position_delta"), MaxPositionDelta);
        LodObj->SetNumberField(TEXT("max_tangent_delta"), MaxTangentDelta);
        Lods.Add(MakeShared<FJsonValueObject>(LodObj));
      }
      Item->SetArrayField(TEXT("lods"), Lods);
      MorphTargets.Add(MakeShared<FJsonValueObject>(Item));
    }
  }
  Obj->SetNumberField(TEXT("morph_target_count"), MorphTargets.Num());
  Obj->SetArrayField(TEXT("morph_targets"), MorphTargets);
  Obj->SetArrayField(TEXT("operations"), {});
  Obj->SetStringField(TEXT("edit_policy"),
                      TEXT("remove_only; raw_delta_summary_only"));
  return Obj;
}
static TSharedPtr<FJsonObject> BuildSocketsJson(USkeletalMesh *Mesh) {
  TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
  TArray<TSharedPtr<FJsonValue>> Sockets;
  for (USkeletalMeshSocket *Socket : Mesh->GetMeshOnlySocketList()) {
    if (Socket) {
      Sockets.Add(MakeShared<FJsonValueObject>(BuildSocketJson(Socket)));
    }
  }
  Obj->SetArrayField(TEXT("sockets"), Sockets);
  return Obj;
}
} // namespace UeAgentSkeletalMeshOps

bool FUeAgentLegacyMechanics::ExecuteSkeletalMeshCommand(
    const FString &CommandLower, const FUeAgentRequestContext &Ctx,
    TSharedPtr<FJsonObject> &OutData, FString &OutError) const {
  if (CommandLower == TEXT("skeletal_mesh_get_info"))
    return CmdSkeletalMeshGetInfo(Ctx, OutData, OutError);
  if (CommandLower == TEXT("skeletal_mesh_export_folder"))
    return CmdSkeletalMeshExportFolder(Ctx, OutData, OutError);
  if (CommandLower == TEXT("skeletal_mesh_apply_folder"))
    return CmdSkeletalMeshApplyFolder(Ctx, OutData, OutError);
  if (CommandLower == TEXT("skeletal_mesh_validate_folder"))
    return CmdSkeletalMeshValidateFolder(Ctx, OutData, OutError);
  if (CommandLower == TEXT("skeletal_mesh_get_morph_targets"))
    return CmdSkeletalMeshGetMorphTargets(Ctx, OutData, OutError);
  if (CommandLower == TEXT("skeletal_mesh_validate_morph_targets"))
    return CmdSkeletalMeshValidateMorphTargets(Ctx, OutData, OutError);
  if (CommandLower == TEXT("skeletal_mesh_preview_morph_target"))
    return CmdSkeletalMeshPreviewMorphTarget(Ctx, OutData, OutError);
  if (CommandLower == TEXT("skeletal_mesh_remove_morph_target"))
    return CmdSkeletalMeshRemoveMorphTarget(Ctx, OutData, OutError);
  if (CommandLower == TEXT("skeletal_mesh_import_skin_weight_profile"))
    return CmdSkeletalMeshImportSkinWeightProfile(Ctx, OutData, OutError);
  if (CommandLower == TEXT("skeletal_mesh_remove_skin_weight_profile"))
    return CmdSkeletalMeshRemoveSkinWeightProfile(Ctx, OutData, OutError);
  if (CommandLower == TEXT("skeletal_mesh_preview_skin_weight_profile"))
    return CmdSkeletalMeshPreviewSkinWeightProfile(Ctx, OutData, OutError);

  OutError = TEXT("unknown_skeletal_mesh_command");
  return false;
}

bool FUeAgentLegacyMechanics::CmdSkeletalMeshGetInfo(
    const FUeAgentRequestContext &Ctx, TSharedPtr<FJsonObject> &OutData,
    FString &OutError) const {
  FString AssetPath;
  if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) ||
      AssetPath.TrimStartAndEnd().IsEmpty()) {
    OutError = TEXT("missing_asset_path");
    return false;
  }
  USkeletalMesh *Mesh = UeAgentSkeletalMeshOps::LoadSkeletalMesh(AssetPath);
  if (!Mesh) {
    OutError = TEXT("skeletal_mesh_not_found");
    return false;
  }

  OutData = UeAgentSkeletalMeshOps::BuildMeshJson(Mesh);
  OutData->SetStringField(TEXT("asset_path"),
                          UeAgentSkeletalMeshOps::NormalizeAssetPath(
                              Mesh->GetOutermost()->GetName()));
  OutData->SetStringField(TEXT("object_path"), Mesh->GetPathName());
  OutData->SetObjectField(
      TEXT("bounds"),
      UeAgentSkeletalMeshOps::BuildBoundsJson(Mesh->GetBounds()));
  OutData->SetObjectField(
      TEXT("imported_bounds"),
      UeAgentSkeletalMeshOps::BuildBoundsJson(Mesh->GetImportedBounds()));
  OutData->SetObjectField(TEXT("materials_json"),
                          UeAgentSkeletalMeshOps::BuildMaterialsJson(Mesh));
  OutData->SetObjectField(TEXT("lods_json"),
                          UeAgentSkeletalMeshOps::BuildLodsIndexJson(Mesh));
  OutData->SetObjectField(TEXT("sockets_json"),
                          UeAgentSkeletalMeshOps::BuildSocketsJson(Mesh));
  return true;
}

bool FUeAgentLegacyMechanics::CmdSkeletalMeshExportFolder(
    const FUeAgentRequestContext &Ctx, TSharedPtr<FJsonObject> &OutData,
    FString &OutError) const {
  return UeAgentAnimationFolderCommandBridge::Export(
      UeAgentAnimationAuthoring::EAssetKind::SkeletalMesh, Ctx.Params, OutData,
      OutError);
}

bool FUeAgentLegacyMechanics::CmdSkeletalMeshApplyFolder(
    const FUeAgentRequestContext &Ctx, TSharedPtr<FJsonObject> &OutData,
    FString &OutError) const {
  return UeAgentAnimationFolderCommandBridge::Apply(
      UeAgentAnimationAuthoring::EAssetKind::SkeletalMesh, Ctx.Params, OutData,
      OutError, Ctx.RequestId);
}

bool FUeAgentLegacyMechanics::CmdSkeletalMeshValidateFolder(
    const FUeAgentRequestContext &Ctx, TSharedPtr<FJsonObject> &OutData,
    FString &OutError) const {
  return UeAgentAnimationFolderCommandBridge::Validate(
      UeAgentAnimationAuthoring::EAssetKind::SkeletalMesh, Ctx.Params, OutData,
      OutError);
}
bool FUeAgentLegacyMechanics::CmdSkeletalMeshGetMorphTargets(
    const FUeAgentRequestContext &Ctx, TSharedPtr<FJsonObject> &OutData,
    FString &OutError) const {
  FString AssetPath;
  if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) ||
      AssetPath.TrimStartAndEnd().IsEmpty()) {
    OutError = TEXT("missing_asset_path");
    return false;
  }
  USkeletalMesh *Mesh = UeAgentSkeletalMeshOps::LoadSkeletalMesh(AssetPath);
  if (!Mesh) {
    OutError = TEXT("skeletal_mesh_not_found");
    return false;
  }
  OutData = UeAgentSkeletalMeshOps::BuildMorphTargetsJson(Mesh);
  OutData->SetStringField(TEXT("asset_path"),
                          UeAgentSkeletalMeshOps::NormalizeAssetPath(
                              Mesh->GetOutermost()->GetName()));
  return true;
}

bool FUeAgentLegacyMechanics::CmdSkeletalMeshValidateMorphTargets(
    const FUeAgentRequestContext &Ctx, TSharedPtr<FJsonObject> &OutData,
    FString &OutError) const {
  if (!CmdSkeletalMeshGetMorphTargets(Ctx, OutData, OutError)) {
    return false;
  }
  TArray<TSharedPtr<FJsonValue>> Issues;
  const TArray<TSharedPtr<FJsonValue>> *MorphTargets = nullptr;
  if (OutData.IsValid() &&
      OutData->TryGetArrayField(TEXT("morph_targets"), MorphTargets) &&
      MorphTargets) {
    for (int32 Index = 0; Index < MorphTargets->Num(); ++Index) {
      TSharedPtr<FJsonObject> MorphObj;
      if (!UeAgentJsonDiagnostics::ReadObjectFromValue(
              (*MorphTargets)[Index],
              FString::Printf(TEXT("morph_targets[%d]"), Index), MorphObj,
              Issues)) {
        continue;
      }
      bool bHasValidData = false;
      MorphObj->TryGetBoolField(TEXT("has_valid_data"), bHasValidData);
      if (!bHasValidData) {
        FString Name;
        MorphObj->TryGetStringField(TEXT("name"), Name);
        UeAgentJsonDiagnostics::AddIssue(
            Issues, TEXT("warning"), TEXT("morph_target_has_no_valid_data"),
            FString::Printf(TEXT("morph_targets[%d]"), Index), Name);
      }
    }
  }
  OutData->SetArrayField(TEXT("validation_issues"), Issues);
  OutData->SetNumberField(TEXT("validation_issue_count"), Issues.Num());
  return true;
}

bool FUeAgentLegacyMechanics::CmdSkeletalMeshPreviewMorphTarget(
    const FUeAgentRequestContext &Ctx, TSharedPtr<FJsonObject> &OutData,
    FString &OutError) const {
  FString AssetPath;
  if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) ||
      AssetPath.TrimStartAndEnd().IsEmpty()) {
    OutError = TEXT("missing_asset_path");
    return false;
  }

  FString MorphName;
  if (!JsonTryGetString(Ctx.Params, TEXT("morph_target"), MorphName) ||
      MorphName.TrimStartAndEnd().IsEmpty()) {
    JsonTryGetString(Ctx.Params, TEXT("name"), MorphName);
  }
  MorphName.TrimStartAndEndInline();
  if (MorphName.IsEmpty()) {
    OutError = TEXT("missing_morph_target");
    return false;
  }

  float Weight = 1.0f;
  double WeightNumber = 1.0;
  if (Ctx.Params->TryGetNumberField(TEXT("weight"), WeightNumber)) {
    Weight = static_cast<float>(WeightNumber);
  }

  USkeletalMesh *Mesh = UeAgentSkeletalMeshOps::LoadSkeletalMesh(AssetPath);
  if (!Mesh) {
    OutError = TEXT("skeletal_mesh_not_found");
    return false;
  }

  UMorphTarget *FoundMorph = nullptr;
  for (const TObjectPtr<UMorphTarget> &MorphTarget : Mesh->GetMorphTargets()) {
    if (MorphTarget &&
        MorphTarget->GetName().Equals(MorphName, ESearchCase::IgnoreCase)) {
      FoundMorph = MorphTarget.Get();
      break;
    }
  }
  if (!FoundMorph) {
    OutError = TEXT("morph_target_not_found");
    return false;
  }

  USkeletalMeshComponent *PreviewComponent =
      NewObject<USkeletalMeshComponent>(GetTransientPackage());
  PreviewComponent->SetSkeletalMesh(Mesh);
  PreviewComponent->SetMorphTarget(FName(*MorphName), Weight);

  OutData = MakeShared<FJsonObject>();
  OutData->SetStringField(TEXT("asset_path"),
                          UeAgentSkeletalMeshOps::NormalizeAssetPath(
                              Mesh->GetOutermost()->GetName()));
  OutData->SetStringField(TEXT("morph_target"), FoundMorph->GetName());
  OutData->SetNumberField(TEXT("requested_weight"), Weight);
  OutData->SetStringField(TEXT("preview_status"),
                          TEXT("transient_component_morph_set"));
  OutData->SetObjectField(TEXT("morph_target_summary"),
                          UeAgentSkeletalMeshOps::BuildMorphTargetsJson(Mesh));
  return true;
}

bool FUeAgentLegacyMechanics::CmdSkeletalMeshRemoveMorphTarget(
    const FUeAgentRequestContext &Ctx, TSharedPtr<FJsonObject> &OutData,
    FString &OutError) const {
  FString AssetPath;
  if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) ||
      AssetPath.TrimStartAndEnd().IsEmpty()) {
    OutError = TEXT("missing_asset_path");
    return false;
  }
  FString MorphName;
  if (!JsonTryGetString(Ctx.Params, TEXT("morph_target"), MorphName) ||
      MorphName.TrimStartAndEnd().IsEmpty()) {
    JsonTryGetString(Ctx.Params, TEXT("name"), MorphName);
  }
  if (MorphName.TrimStartAndEnd().IsEmpty()) {
    JsonTryGetString(Ctx.Params, TEXT("morph_target_name"), MorphName);
  }
  if (MorphName.TrimStartAndEnd().IsEmpty()) {
    OutError = TEXT("missing_morph_target");
    return false;
  }
  bool bAllowDestructive = false;
  JsonTryGetBool(Ctx.Params, TEXT("allow_destructive"), bAllowDestructive);
  if (!bAllowDestructive) {
    OutError = TEXT("allow_destructive_required");
    return false;
  }
  USkeletalMesh *Mesh = UeAgentSkeletalMeshOps::LoadSkeletalMesh(AssetPath);
  if (!Mesh) {
    OutError = TEXT("skeletal_mesh_not_found");
    return false;
  }
  Mesh->Modify();
  TArray<FName> MorphNames;
  MorphNames.Add(FName(*MorphName.TrimStartAndEnd()));
  const bool bRemoved = Mesh->RemoveMorphTargets(MorphNames);
  if (!bRemoved) {
    OutError = TEXT("morph_target_remove_failed");
    return false;
  }
  Mesh->MarkPackageDirty();
  bool bSaveAfterApply = false;
  JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSaveAfterApply);
  if (!bSaveAfterApply) {
    JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterApply);
  }
  if (bSaveAfterApply &&
      !UeAgentSkeletalMeshOps::SaveAssetPackage(Mesh, OutError)) {
    return false;
  }
  OutData->SetStringField(TEXT("asset_path"),
                          UeAgentSkeletalMeshOps::NormalizeAssetPath(
                              Mesh->GetOutermost()->GetName()));
  OutData->SetStringField(TEXT("morph_target"), MorphName.TrimStartAndEnd());
  OutData->SetBoolField(TEXT("removed"), true);
  OutData->SetBoolField(TEXT("saved"), bSaveAfterApply);
  return true;
}

bool FUeAgentLegacyMechanics::CmdSkeletalMeshImportSkinWeightProfile(
    const FUeAgentRequestContext &Ctx, TSharedPtr<FJsonObject> &OutData,
    FString &OutError) const {
  FString AssetPath;
  if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) ||
      AssetPath.TrimStartAndEnd().IsEmpty()) {
    OutError = TEXT("missing_asset_path");
    return false;
  }

  FString SourceFilename;
  if (!JsonTryGetString(Ctx.Params, TEXT("source_filename"), SourceFilename) ||
      SourceFilename.TrimStartAndEnd().IsEmpty()) {
    JsonTryGetString(Ctx.Params, TEXT("source_file"), SourceFilename);
  }
  SourceFilename.TrimStartAndEndInline();
  if (SourceFilename.IsEmpty()) {
    OutError = TEXT("missing_source_filename");
    return false;
  }
  if (FPaths::IsRelative(SourceFilename)) {
    SourceFilename = FPaths::ConvertRelativePathToFull(
        FPaths::Combine(FPaths::ProjectDir(), SourceFilename));
  }
  if (!FPaths::FileExists(SourceFilename)) {
    OutError = TEXT("source_file_not_found");
    return false;
  }

  FString ProfileNameString;
  JsonTryGetString(Ctx.Params, TEXT("profile_name"), ProfileNameString);
  ProfileNameString.TrimStartAndEndInline();
  if (ProfileNameString.IsEmpty()) {
    OutError = TEXT("missing_profile_name");
    return false;
  }

  int32 LodIndex = 0;
  double InputLodIndex = 0.0;
  if (JsonTryGetNumber(Ctx.Params, TEXT("lod_index"), InputLodIndex)) {
    LodIndex = static_cast<int32>(InputLodIndex);
  }

  USkeletalMesh *Mesh = UeAgentSkeletalMeshOps::LoadSkeletalMesh(AssetPath);
  if (!Mesh) {
    OutError = TEXT("skeletal_mesh_not_found");
    return false;
  }
  if (LodIndex < 0 || LodIndex >= Mesh->GetLODNum()) {
    OutError = TEXT("invalid_lod_index");
    return false;
  }

  bool bSaveAfterImport = false;
  JsonTryGetBool(Ctx.Params, TEXT("save_after_import"), bSaveAfterImport);

  Mesh->Modify();
  const FName ProfileName(*ProfileNameString);
  const bool bImported = FSkinWeightsUtilities::ImportAlternateSkinWeight(
      Mesh, SourceFilename, LodIndex, ProfileName, /*bIsReimport*/ false);
  Mesh->PostEditChange();
  Mesh->MarkPackageDirty();

  OutData = MakeShared<FJsonObject>();
  OutData->SetStringField(TEXT("asset_path"),
                          UeAgentSkeletalMeshOps::NormalizeAssetPath(
                              Mesh->GetOutermost()->GetName()));
  OutData->SetStringField(TEXT("source_filename"), SourceFilename);
  OutData->SetStringField(TEXT("profile_name"), ProfileName.ToString());
  OutData->SetNumberField(TEXT("lod_index"), LodIndex);
  OutData->SetBoolField(TEXT("imported"), bImported);
  OutData->SetObjectField(
      TEXT("readback"),
      UeAgentSkeletalMeshOps::BuildSkinWeightProfilesJson(Mesh));
  if (!bImported) {
    OutError = TEXT("skin_weight_profile_import_failed");
    return false;
  }

  if (bSaveAfterImport &&
      !UeAgentSkeletalMeshOps::SaveAssetPackage(Mesh, OutError)) {
    return false;
  }
  OutData->SetBoolField(TEXT("saved"), bSaveAfterImport);
  return true;
}

bool FUeAgentLegacyMechanics::CmdSkeletalMeshRemoveSkinWeightProfile(
    const FUeAgentRequestContext &Ctx, TSharedPtr<FJsonObject> &OutData,
    FString &OutError) const {
  FString AssetPath;
  if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) ||
      AssetPath.TrimStartAndEnd().IsEmpty()) {
    OutError = TEXT("missing_asset_path");
    return false;
  }

  FString ProfileNameString;
  JsonTryGetString(Ctx.Params, TEXT("profile_name"), ProfileNameString);
  ProfileNameString.TrimStartAndEndInline();
  if (ProfileNameString.IsEmpty()) {
    OutError = TEXT("missing_profile_name");
    return false;
  }
  bool bAllowDestructive = false;
  JsonTryGetBool(Ctx.Params, TEXT("allow_destructive"), bAllowDestructive);
  if (!bAllowDestructive) {
    OutError = TEXT("allow_destructive_required");
    return false;
  }

  USkeletalMesh *Mesh = UeAgentSkeletalMeshOps::LoadSkeletalMesh(AssetPath);
  if (!Mesh) {
    OutError = TEXT("skeletal_mesh_not_found");
    return false;
  }

  const FName ProfileName(*ProfileNameString);
  const bool bProfileExists = Mesh->GetSkinWeightProfiles().ContainsByPredicate(
      [ProfileName](const FSkinWeightProfileInfo &Profile) {
        return Profile.Name == ProfileName;
      });
  if (!bProfileExists) {
    OutError = TEXT("skin_weight_profile_not_found");
    return false;
  }

  int32 LodIndex = INDEX_NONE;
  double InputLodIndex = static_cast<double>(INDEX_NONE);
  if (JsonTryGetNumber(Ctx.Params, TEXT("lod_index"), InputLodIndex)) {
    LodIndex = static_cast<int32>(InputLodIndex);
    if (LodIndex < 0 || LodIndex >= Mesh->GetLODNum()) {
      OutError = TEXT("invalid_lod_index");
      return false;
    }
  }

  bool bSaveAfterApply = false;
  JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSaveAfterApply);

  Mesh->Modify();
  int32 RemovedLodCount = 0;
  if (LodIndex == INDEX_NONE) {
    for (int32 CurrentLod = 0; CurrentLod < Mesh->GetLODNum(); ++CurrentLod) {
      if (FSkinWeightsUtilities::RemoveSkinnedWeightProfileData(
              Mesh, ProfileName, CurrentLod)) {
        ++RemovedLodCount;
      }
    }
    Mesh->GetSkinWeightProfiles().RemoveAll(
        [ProfileName](const FSkinWeightProfileInfo &Profile) {
          return Profile.Name == ProfileName;
        });
  } else {
    if (FSkinWeightsUtilities::RemoveSkinnedWeightProfileData(Mesh, ProfileName,
                                                              LodIndex)) {
      ++RemovedLodCount;
    }
    for (FSkinWeightProfileInfo &Profile : Mesh->GetSkinWeightProfiles()) {
      if (Profile.Name == ProfileName) {
        Profile.PerLODSourceFiles.Remove(LodIndex);
        break;
      }
    }
  }

  Mesh->ReleaseSkinWeightProfileResources();
  Mesh->PostEditChange();
  Mesh->MarkPackageDirty();

  if (bSaveAfterApply &&
      !UeAgentSkeletalMeshOps::SaveAssetPackage(Mesh, OutError)) {
    return false;
  }

  OutData = MakeShared<FJsonObject>();
  OutData->SetStringField(TEXT("asset_path"),
                          UeAgentSkeletalMeshOps::NormalizeAssetPath(
                              Mesh->GetOutermost()->GetName()));
  OutData->SetStringField(TEXT("profile_name"), ProfileName.ToString());
  OutData->SetNumberField(TEXT("lod_index"), LodIndex);
  OutData->SetNumberField(TEXT("removed_lod_count"), RemovedLodCount);
  OutData->SetBoolField(TEXT("removed_profile_record"), LodIndex == INDEX_NONE);
  OutData->SetBoolField(TEXT("saved"), bSaveAfterApply);
  OutData->SetObjectField(
      TEXT("readback"),
      UeAgentSkeletalMeshOps::BuildSkinWeightProfilesJson(Mesh));
  return true;
}

bool FUeAgentLegacyMechanics::CmdSkeletalMeshPreviewSkinWeightProfile(
    const FUeAgentRequestContext &Ctx, TSharedPtr<FJsonObject> &OutData,
    FString &OutError) const {
  FString AssetPath;
  if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) ||
      AssetPath.TrimStartAndEnd().IsEmpty()) {
    OutError = TEXT("missing_asset_path");
    return false;
  }

  FString ProfileNameString;
  JsonTryGetString(Ctx.Params, TEXT("profile_name"), ProfileNameString);
  ProfileNameString.TrimStartAndEndInline();
  if (ProfileNameString.IsEmpty()) {
    OutError = TEXT("missing_profile_name");
    return false;
  }

  USkeletalMesh *Mesh = UeAgentSkeletalMeshOps::LoadSkeletalMesh(AssetPath);
  if (!Mesh) {
    OutError = TEXT("skeletal_mesh_not_found");
    return false;
  }

  const FName ProfileName(*ProfileNameString);
  const bool bProfileExists = Mesh->GetSkinWeightProfiles().ContainsByPredicate(
      [ProfileName](const FSkinWeightProfileInfo &Profile) {
        return Profile.Name == ProfileName;
      });
  if (!bProfileExists) {
    OutError = TEXT("skin_weight_profile_not_found");
    return false;
  }

  USkeletalMeshComponent *PreviewComponent =
      NewObject<USkeletalMeshComponent>(GetTransientPackage());
  PreviewComponent->SetSkeletalMesh(Mesh);
  const bool bSet = PreviewComponent->SetSkinWeightProfile(ProfileName);

  OutData = MakeShared<FJsonObject>();
  OutData->SetStringField(TEXT("asset_path"),
                          UeAgentSkeletalMeshOps::NormalizeAssetPath(
                              Mesh->GetOutermost()->GetName()));
  OutData->SetStringField(TEXT("profile_name"), ProfileName.ToString());
  OutData->SetBoolField(TEXT("preview_profile_set"), bSet);
  OutData->SetStringField(
      TEXT("current_profile"),
      PreviewComponent->GetCurrentSkinWeightProfileName().ToString());
  OutData->SetStringField(TEXT("preview_status"),
                          bSet ? TEXT("transient_component_profile_set")
                               : TEXT("component_rejected_profile"));
  OutData->SetObjectField(
      TEXT("skin_weight_profiles"),
      UeAgentSkeletalMeshOps::BuildSkinWeightProfilesJson(Mesh));
  return bSet;
}
