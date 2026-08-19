// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringDescriptors.h"

#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticRegistry.h"

namespace UeAgentReflectionAuthoring {
namespace {
FProfile Profile(const TCHAR *TypeId, const TCHAR *DisplayName,
                 const TCHAR *AssetClassPath,
                 const TCHAR *CreateClassPath = nullptr,
                 const bool bAllowCreate = true,
                 const TCHAR *ProfileStateField = nullptr,
                 const TCHAR *ProfileStateInputType = nullptr) {
  FProfile Result;
  Result.TypeId = TypeId;
  Result.DisplayName = DisplayName;
  Result.TargetKind = FString::Printf(TEXT("reflection.%s"), TypeId);
  Result.AssetClassPath = AssetClassPath;
  Result.CreateClassPath = CreateClassPath ? CreateClassPath : AssetClassPath;
  Result.IdentityModelId =
      FString::Printf(TEXT("reflection.%s.identity"), TypeId);
  Result.SeedSemanticId =
      FString::Printf(TEXT("uai.semantic.reflection.%s.seed"), TypeId);
  Result.ExportTargetSemanticId =
      FString::Printf(TEXT("uai.semantic.reflection.%s.export.target"), TypeId);
  if (ProfileStateField && FCString::Strlen(ProfileStateField) > 0) {
    Result.ProfileStateSemanticId = FString::Printf(
        TEXT("uai.semantic.reflection.%s.profile_state"), TypeId);
    Result.ProfileStateField = ProfileStateField;
    Result.ProfileStateInputType =
        ProfileStateInputType ? ProfileStateInputType : TEXT("json_value");
  }
  Result.InputContractId =
      FString::Printf(TEXT("uai.apply_input.reflection.%s"), TypeId);
  Result.WorkspaceContractId =
      FString::Printf(TEXT("uai.apply_workspace.reflection.%s"), TypeId);
  Result.ExportContractId =
      FString::Printf(TEXT("uai.export.reflection.%s"), TypeId);
  Result.bAllowCreate = bAllowCreate;
  return Result;
}

FProfile PaperSpriteProfile() {
  FProfile Result = Profile(TEXT("paper_sprite"), TEXT("Paper Sprite"),
                            TEXT("/Script/Paper2D.PaperSprite"));
  Result.ExcludedRootProperties = {TEXT("BodySetup")};
  return Result;
}

const TArray<FProfile> Profiles = {
    Profile(TEXT("deformer_graph"), TEXT("Deformer Graph"),
            TEXT("/Script/OptimusCore.OptimusDeformer")),
    Profile(TEXT("physics_asset"), TEXT("Physics Asset"),
            TEXT("/Script/Engine.PhysicsAsset")),
    Profile(TEXT("static_mesh"), TEXT("Static Mesh"),
            TEXT("/Script/Engine.StaticMesh"), nullptr, false),
    Profile(TEXT("behavior_tree"), TEXT("Behavior Tree"),
            TEXT("/Script/AIModule.BehaviorTree"), nullptr, true, TEXT("root"),
            TEXT("json_value")),
    Profile(TEXT("state_tree"), TEXT("State Tree"),
            TEXT("/Script/StateTreeModule.StateTree"), nullptr, true,
            TEXT("state_tree"), TEXT("json_object")),
    Profile(TEXT("eqs"), TEXT("Environment Query"),
            TEXT("/Script/AIModule.EnvQuery"), nullptr, true, TEXT("options"),
            TEXT("json_array")),
    Profile(TEXT("sequence"), TEXT("Level Sequence"),
            TEXT("/Script/LevelSequence.LevelSequence"), nullptr, true,
            TEXT("movie_scene"), TEXT("json_object")),
    Profile(TEXT("media_player"), TEXT("Media Player"),
            TEXT("/Script/MediaAssets.MediaPlayer")),
    Profile(TEXT("media_source"), TEXT("Media Source"),
            TEXT("/Script/MediaAssets.MediaSource"),
            TEXT("/Script/MediaAssets.FileMediaSource")),
    Profile(TEXT("media_texture"), TEXT("Media Texture"),
            TEXT("/Script/MediaAssets.MediaTexture")),
    Profile(TEXT("paper_flipbook"), TEXT("Paper Flipbook"),
            TEXT("/Script/Paper2D.PaperFlipbook")),
    PaperSpriteProfile(),
    Profile(TEXT("paper_tilemap"), TEXT("Paper Tile Map"),
            TEXT("/Script/Paper2D.PaperTileMap")),
    Profile(TEXT("paper_tileset"), TEXT("Paper Tile Set"),
            TEXT("/Script/Paper2D.PaperTileSet")),
    Profile(TEXT("render_target"), TEXT("Render Target"),
            TEXT("/Script/Engine.TextureRenderTarget"),
            TEXT("/Script/Engine.TextureRenderTarget2D")),
    Profile(TEXT("runtime_virtual_texture"), TEXT("Runtime Virtual Texture"),
            TEXT("/Script/Engine.RuntimeVirtualTexture")),
    Profile(TEXT("sparse_volume_texture"), TEXT("Sparse Volume Texture"),
            TEXT("/Script/Engine.SparseVolumeTexture"), nullptr, false),
    Profile(TEXT("subuv_animation"), TEXT("SubUV Animation"),
            TEXT("/Script/Engine.SubUVAnimation")),
    Profile(TEXT("texture_array"), TEXT("Texture 2D Array"),
            TEXT("/Script/Engine.Texture2DArray")),
    Profile(TEXT("texture_collection"), TEXT("Texture Collection"),
            TEXT("/Script/Engine.TextureCollection")),
    Profile(TEXT("texture_cube"), TEXT("Texture Cube"),
            TEXT("/Script/Engine.TextureCube")),
    Profile(TEXT("texture_cube_array"), TEXT("Texture Cube Array"),
            TEXT("/Script/Engine.TextureCubeArray")),
    Profile(TEXT("texture_graph"), TEXT("Texture Graph"),
            TEXT("/Script/TextureGraph.TextureGraph"), nullptr, true,
            TEXT("graph"), TEXT("json_object")),
    Profile(TEXT("volume_texture"), TEXT("Volume Texture"),
            TEXT("/Script/Engine.VolumeTexture")),
    Profile(TEXT("material_instance"), TEXT("Material Instance"),
            TEXT("/Script/Engine.MaterialInstanceConstant")),
};

bool RegisterIdentity(const FProfile &Profile, FString &OutError) {
  UeAgentAuthoringIdentity::FModelDescriptor Model;
  Model.ModelId = Profile.IdentityModelId;
  Model.TargetKind = Profile.TargetKind;
  Model.Selectors = {{TEXT("asset_path"), 100, false}};
  Model.KeyPatterns = {{TEXT("asset"), 100, {TEXT("asset_path")}}};
  UeAgentAuthoringIdentity::ERegistrationResult Result;
  return UeAgentAuthoringIdentity::FRegistry::Get().RegisterModel(Model, Result,
                                                                  OutError);
}

bool RegisterSemantic(
    const FProfile &Profile, const FString &SemanticId,
    const UeAgentAuthoringSemanticRegistry::EWriteCapability WriteCapability,
    const bool bFactoryDefault, FString &OutError) {
  UeAgentAuthoringSemanticRegistry::FSemanticDescriptor Semantic;
  Semantic.SemanticId = SemanticId;
  Semantic.DomainId = DomainId;
  Semantic.TargetKind = Profile.TargetKind;
  if (!Profile.ProfileStateSemanticId.IsEmpty() &&
      SemanticId == Profile.ProfileStateSemanticId) {
    Semantic.ValueType =
        Profile.ProfileStateInputType == TEXT("json_array")
            ? TEXT("array")
            : (Profile.ProfileStateInputType == TEXT("json_value")
                   ? TEXT("*")
                   : TEXT("object"));
  } else {
    Semantic.ValueType = TEXT("object");
  }
  Semantic.OwnerModel = TEXT("reflection_asset_owner");
  Semantic.IdentityModel = Profile.IdentityModelId;
  Semantic.StateAccessor = StateAccessorId;
  Semantic.WriteCapability = WriteCapability;
  Semantic.ConstraintSource = TEXT("reflection.asset.class");
  Semantic.AvailabilitySource = StateAccessorId;
  Semantic.Comparator = TEXT("uai.compare.canonical_value");
  Semantic.CapabilityTags = {TEXT("reflection"), TEXT("asset"), Profile.TypeId,
                             TEXT("editor_authoring")};
  if (bFactoryDefault) {
    Semantic.DefaultSourceKind =
        UeAgentAuthoringSemanticRegistry::EDefaultSourceKind::Factory;
    Semantic.DefaultAccessor = FactoryDefaultProviderId;
  }
  UeAgentAuthoringSemanticRegistry::ERegistrationResult Result;
  return UeAgentAuthoringSemanticRegistry::FRegistry::Get().RegisterSemantic(
      Semantic, Result, OutError);
}
} // namespace

const TArray<FProfile> &GetProfiles() { return Profiles; }

const FProfile *FindProfileByTypeId(const FString &TypeId) {
  return Profiles.FindByPredicate([&TypeId](const FProfile &Item) {
    return Item.TypeId.Equals(TypeId, ESearchCase::IgnoreCase);
  });
}

const FProfile *FindProfileByTargetKind(const FString &TargetKind) {
  return Profiles.FindByPredicate([&TargetKind](const FProfile &Item) {
    return Item.TargetKind.Equals(TargetKind, ESearchCase::IgnoreCase);
  });
}

bool RegisterDescriptors(FString &OutError) {
  for (const FProfile &Item : Profiles) {
    if (!RegisterIdentity(Item, OutError))
      return false;
  }

  UeAgentAuthoringSemanticRegistry::FDomainDescriptor Domain;
  Domain.DomainId = DomainId;
  Domain.MutationProviderId = ApplyProviderId;
  for (const FProfile &Item : Profiles)
    Domain.TargetKinds.Add(Item.TargetKind);
  Domain.CapabilityTags = {TEXT("reflection"), TEXT("asset_lifecycle"),
                           TEXT("recursive_object"), TEXT("editor_authoring")};
  UeAgentAuthoringSemanticRegistry::ERegistrationResult DomainResult;
  if (!UeAgentAuthoringSemanticRegistry::FRegistry::Get().RegisterDomain(
          Domain, DomainResult, OutError))
    return false;

  using EWrite = UeAgentAuthoringSemanticRegistry::EWriteCapability;
  for (const FProfile &Item : Profiles) {
    if (!RegisterSemantic(Item, Item.SeedSemanticId, EWrite::DomainMutation,
                          true, OutError) ||
        !RegisterSemantic(Item, Item.ExportTargetSemanticId, EWrite::ReadOnly,
                          false, OutError) ||
        (!Item.ProfileStateSemanticId.IsEmpty() &&
         !RegisterSemantic(Item, Item.ProfileStateSemanticId,
                           EWrite::DomainMutation, false, OutError))) {
      return false;
    }
  }
  return true;
}
} // namespace UeAgentReflectionAuthoring
