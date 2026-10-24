// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringDescriptors.h"
#include "Domains/BehaviorTree/Authoring/UeAgentBehaviorTreeAuthoringState.h"
#include "Domains/Blackboard/Authoring/UeAgentBlackboardAuthoringState.h"
#include "Domains/Eqs/Authoring/UeAgentEqsAuthoringState.h"
#include "Domains/StateTree/Authoring/UeAgentStateTreeAuthoringState.h"
#include "Domains/SmartObject/Authoring/UeAgentSmartObjectAuthoringState.h"

#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticRegistry.h"
#include "UObject/UnrealType.h"
#include "Engine/DataAsset.h"
#include "SparseVolumeTexture/SparseVolumeTexture.h"

namespace UeAgentReflectionAuthoring {
bool FProfile::AuthorizesProperty(const FProperty* Property) const {
  return Property && !Property->HasAnyPropertyFlags(CPF_EditConst | CPF_Transient | CPF_Deprecated
      | CPF_DuplicateTransient | CPF_NonPIEDuplicateTransient | CPF_TextExportTransient)
      && !Property->HasMetaData(TEXT("DeprecatedProperty"))
      && (AuthorizedPropertyPaths.Contains(Property->GetPathName())
          || (AdditionalPropertyAuthorization && AdditionalPropertyAuthorization(Property)));
}
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

FProfile TextureSourceProfile(const TCHAR* TypeId, const TCHAR* DisplayName, const TCHAR* ClassPath) {
  FProfile Result = Profile(TypeId, DisplayName, ClassPath);
  Result.GenerationInputSemanticId = FString::Printf(TEXT("uai.semantic.reflection.%s.generation_input"), TypeId);
  Result.GenerationInputField = TEXT("source_input");
  Result.GeneratedStateSemanticId = FString::Printf(TEXT("uai.semantic.reflection.%s.generated_state"), TypeId);
  return Result;
}

FProfile MlDeformerProfile() {
  FProfile Result = Profile(TEXT("ml_deformer"), TEXT("ML Deformer"),
      TEXT("/Script/MLDeformerFramework.MLDeformerAsset"), nullptr, false,
      TEXT("model_settings"), TEXT("json_object"));
  Result.ExcludedRootProperties = {TEXT("Model")};
  Result.bProfileStateAsProperty = true;
  return Result;
}

FProfile StaticMeshProfile() {
  FProfile Result = Profile(TEXT("static_mesh"), TEXT("Static Mesh"),
      TEXT("/Script/Engine.StaticMesh"), nullptr, false, TEXT("mesh"), TEXT("json_object"));
  // The native material editor exposes this otherwise non-Edit container.
  // Keep its values, references and recursive field authorization in the shared owner.
  Result.AuthorizedPropertyPaths = {TEXT("/Script/Engine.StaticMesh:StaticMaterials")};
  // Native serialization owns this transient subobject. Its settings are exposed
  // once by mesh state, never again through the root's inline-object projection.
  Result.ExcludedRootProperties = {TEXT("BodySetup")};
  Result.bProfileStateAsProperty = true;
  return Result;
}

FProfile MaterialCollectionProfile() {
  FProfile Result = Profile(TEXT("material_parameter_collection"), TEXT("Material Parameter Collection"),
      TEXT("/Script/Engine.MaterialParameterCollection"), nullptr, false, TEXT("parameters"), TEXT("json_object"));
  Result.ExcludedRootProperties = {TEXT("ScalarParameters"), TEXT("VectorParameters")};
  Result.bProfileStateAsProperty = true;
  return Result;
}

FProfile PcgDataAssetProfile() {
  FProfile Result = Profile(TEXT("pcg_data_asset"), TEXT("PCG Data Asset"),
      TEXT("/Script/PCG.PCGDataAsset"), nullptr, true, TEXT("data"), TEXT("json_object"));
  Result.ExcludedRootProperties = {TEXT("Data"), TEXT("ObjectPath")};
  Result.bProfileStateAsProperty = true;
  return Result;
}

FProfile MaterialFunctionInstanceProfile(const TCHAR* Type, const TCHAR* Name, const TCHAR* Class) {
  FProfile Result = Profile(Type, Name, Class, nullptr, false, TEXT("parameters"), TEXT("json_array"));
  Result.ExcludedRootProperties = {TEXT("ScalarParameterValues"), TEXT("VectorParameterValues"),
      TEXT("DoubleVectorParameterValues"), TEXT("TextureParameterValues"), TEXT("TextureCollectionParameterValues"),
      TEXT("FontParameterValues"), TEXT("RuntimeVirtualTextureParameterValues"), TEXT("SparseVolumeTextureParameterValues"),
      TEXT("StaticSwitchParameterValues"), TEXT("StaticComponentMaskParameterValues")};
  Result.bProfileStateAsProperty = true;
  return Result;
}

FProfile SkeletalMeshProfile() {
  FProfile Result = Profile(TEXT("skeletal_mesh"), TEXT("Skeletal Mesh"),
      TEXT("/Script/Engine.SkeletalMesh"), nullptr, false, TEXT("mesh"), TEXT("json_object"));
  Result.ExcludedRootProperties = {TEXT("Materials"), TEXT("LODSettings"), TEXT("Skeleton")};
  Result.bProfileStateAsProperty = true;
  return Result;
}

FProfile PhysicsAssetProfile() {
  FProfile Result = Profile(TEXT("physics_asset"), TEXT("Physics Asset"),
      TEXT("/Script/Engine.PhysicsAsset"), nullptr, true, TEXT("physics"), TEXT("json_object"));
  Result.ExcludedRootProperties = {TEXT("SkeletalBodySetups"), TEXT("ConstraintSetup"),
      TEXT("PhysicalAnimationProfiles"), TEXT("ConstraintProfiles")};
  Result.bProfileStateAsProperty = true;
  return Result;
}

FProfile PaperSpriteProfile() {
  FProfile Result = Profile(TEXT("paper_sprite"), TEXT("Paper Sprite"),
                            TEXT("/Script/Paper2D.PaperSprite"), nullptr, true, TEXT("collision"), TEXT("json_object"));
  Result.ExcludedRootProperties = {TEXT("BodySetup")};
  Result.bProfileStateAsProperty = true;
  return Result;
}

FProfile SoundClassProfile() {
  FProfile Result = Profile(TEXT("sound_class"), TEXT("Sound Class"),
      TEXT("/Script/Engine.SoundClass"), nullptr, false, TEXT("child_classes"), TEXT("json_array"));
  Result.ExcludedRootProperties = {TEXT("ChildClasses")};
  return Result;
}

FProfile PaperTileSetProfile() {
  FProfile Result = Profile(TEXT("paper_tileset"), TEXT("Paper Tile Set"), TEXT("/Script/Paper2D.PaperTileSet"));
  Result.AuthorizedPropertyPaths = {TEXT("/Script/Paper2D.PaperTileSet:Terrains"),
      TEXT("/Script/Paper2D.PaperTileSetTerrain:CenterTileIndex")};
  return Result;
}

FProfile PaperSpriteAtlasProfile() {
  FProfile Result = Profile(TEXT("paper_sprite_atlas"), TEXT("Paper Sprite Atlas"),
      TEXT("/Script/Paper2D.PaperSpriteAtlas"));
  // Pages and identity are generated by the native atlas builder. A rebuild
  // command is not a persistent setting in an authoring document.
  Result.ExcludedRootProperties = {TEXT("GeneratedTextures"), TEXT("AtlasGUID"), TEXT("bRebuildAtlas")};
  return Result;
}

FProfile SoundSubmixProfile() {
  FProfile Result = Profile(TEXT("sound_submix"), TEXT("Sound Submix"),
      TEXT("/Script/Engine.SoundSubmix"), nullptr, false, TEXT("child_submixes"), TEXT("json_array"));
  Result.ExcludedRootProperties = {TEXT("ChildSubmixes"), TEXT("ParentSubmix")};
  return Result;
}

FProfile DataObjectProfile() {
  FProfile Result = Profile(TEXT("data_object"), TEXT("Data Object"), TEXT("/Script/Engine.DataAsset"), nullptr, false);
  Result.bFallbackDomain = true;
  Result.AcceptsConcreteClass = [](const UClass* Class) {
    return Class && Class->IsChildOf(UDataAsset::StaticClass())
        && !Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists);
  };
  Result.AdditionalPropertyAuthorization = [](const FProperty* Property) {
    return Property && Property->HasMetaData(TEXT("UAIAssetDataField"));
  };
  return Result;
}

FProfile SparseVolumeTextureProfile() {
  FProfile Result = Profile(TEXT("sparse_volume_texture"), TEXT("Sparse Volume Texture"),
      TEXT("/Script/Engine.SparseVolumeTexture"), nullptr, false);
  // Frames are internal data, not separately editable asset roots. Static and
  // animated assets share the native streamable settings and property lifecycle.
  Result.AcceptsConcreteClass = [](const UClass* Class) {
    return Class && Class->IsChildOf(UStreamableSparseVolumeTexture::StaticClass());
  };
  return Result;
}

const TArray<FProfile> Profiles = {
	PcgDataAssetProfile(),
	MaterialCollectionProfile(),
	MaterialFunctionInstanceProfile(TEXT("material_function_instance"), TEXT("Material Function Instance"), TEXT("/Script/Engine.MaterialFunctionInstance")),
	MaterialFunctionInstanceProfile(TEXT("material_layer_instance"), TEXT("Material Layer Instance"), TEXT("/Script/Engine.MaterialFunctionMaterialLayerInstance")),
	MaterialFunctionInstanceProfile(TEXT("material_layer_blend_instance"), TEXT("Material Layer Blend Instance"), TEXT("/Script/Engine.MaterialFunctionMaterialLayerBlendInstance")),
	DataObjectProfile(),
	Profile(TEXT("physical_material"), TEXT("Physical Material"),
		TEXT("/Script/PhysicsCore.PhysicalMaterial"), nullptr, false),
	Profile(TEXT("recursive_dataasset"), TEXT("Recursive Data Asset"),
		TEXT("/Script/UeAgentInterface.UeAgentRecursiveDataAsset"), nullptr, false),
	Profile(TEXT("curve_float"), TEXT("Curve Float"),
		TEXT("/Script/Engine.CurveFloat"), nullptr, false, TEXT("curve"), TEXT("json_object")),
	Profile(TEXT("curve_linear_color"), TEXT("Curve Linear Color"),
		TEXT("/Script/Engine.CurveLinearColor"), nullptr, false, TEXT("curve"), TEXT("json_object")),
	Profile(TEXT("curve_vector"), TEXT("Curve Vector"),
		TEXT("/Script/Engine.CurveVector"), nullptr, false, TEXT("curve"), TEXT("json_object")),
	Profile(TEXT("specular_profile"), TEXT("Specular Profile"),
		TEXT("/Script/Engine.SpecularProfile"), nullptr, false),
    Profile(TEXT("input_action"), TEXT("Input Action"),
            TEXT("/Script/EnhancedInput.InputAction"), nullptr, false),
    Profile(TEXT("input_mapping_context"), TEXT("Input Mapping Context"),
            TEXT("/Script/EnhancedInput.InputMappingContext"), nullptr, false),
    Profile(TEXT("sound_attenuation"), TEXT("Sound Attenuation"),
            TEXT("/Script/Engine.SoundAttenuation"), nullptr, false),
    Profile(TEXT("sound_concurrency"), TEXT("Sound Concurrency"),
            TEXT("/Script/Engine.SoundConcurrency"), nullptr, false),
    SoundClassProfile(),
    SoundSubmixProfile(),
    Profile(TEXT("sound_mix"), TEXT("Sound Mix"),
            TEXT("/Script/Engine.SoundMix"), nullptr, false),
    Profile(TEXT("audio_effect_preset"), TEXT("Submix EQ Preset"),
            TEXT("/Script/AudioMixer.SubmixEffectSubmixEQPreset"), nullptr, false),
    Profile(TEXT("deformer_graph"), TEXT("Deformer Graph"),
            TEXT("/Script/OptimusCore.OptimusDeformer")),
    Profile(TEXT("deformer_source"), TEXT("Deformer Source Library"),
            TEXT("/Script/OptimusCore.OptimusSource")),
    MlDeformerProfile(),
    Profile(TEXT("mesh_deformer_collection"), TEXT("Mesh Deformer Collection"),
            TEXT("/Script/Engine.MeshDeformerCollection")),
    PhysicsAssetProfile(),
    Profile(TEXT("hlod_layer"), TEXT("HLOD Layer"),
            TEXT("/Script/Engine.HLODLayer")),
    StaticMeshProfile(),
    SkeletalMeshProfile(),
    Profile(TEXT("behavior_tree"), TEXT("Behavior Tree"),
            TEXT("/Script/AIModule.BehaviorTree"), nullptr, true, TEXT("graph"),
            TEXT("json_object")),
    Profile(TEXT("blackboard"), TEXT("Blackboard"),
            TEXT("/Script/AIModule.BlackboardData"), nullptr, true,
            TEXT("blackboard"), TEXT("json_object")),
    Profile(TEXT("state_tree"), TEXT("State Tree"),
            TEXT("/Script/StateTreeModule.StateTree"), nullptr, true,
            TEXT("state_tree"), TEXT("json_object")),
    Profile(TEXT("smart_object"), TEXT("Smart Object Definition"),
            TEXT("/Script/SmartObjectsModule.SmartObjectDefinition"), nullptr, true,
            TEXT("smart_object"), TEXT("json_object")),
    Profile(TEXT("eqs"), TEXT("Environment Query"),
            TEXT("/Script/AIModule.EnvQuery"), nullptr, true, TEXT("graph"),
            TEXT("json_object")),
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
    PaperSpriteAtlasProfile(),
    Profile(TEXT("paper_tilemap"), TEXT("Paper Tile Map"),
            TEXT("/Script/Paper2D.PaperTileMap")),
    PaperTileSetProfile(),
    Profile(TEXT("render_target"), TEXT("Render Target"),
            TEXT("/Script/Engine.TextureRenderTarget"),
            TEXT("/Script/Engine.TextureRenderTarget2D")),
    Profile(TEXT("runtime_virtual_texture"), TEXT("Runtime Virtual Texture"),
            TEXT("/Script/Engine.RuntimeVirtualTexture")),
    SparseVolumeTextureProfile(),
    Profile(TEXT("subuv_animation"), TEXT("SubUV Animation"),
            TEXT("/Script/Engine.SubUVAnimation")),
    Profile(TEXT("texture_2d"), TEXT("Texture 2D"),
            TEXT("/Script/Engine.Texture2D"), nullptr, false),
    Profile(TEXT("sound_wave"), TEXT("Sound Wave"),
            TEXT("/Script/Engine.SoundWave"), nullptr, false),
    Profile(TEXT("texture_array"), TEXT("Texture 2D Array"),
            TEXT("/Script/Engine.Texture2DArray")),
    Profile(TEXT("texture_collection"), TEXT("Texture Collection"),
            TEXT("/Script/Engine.TextureCollection")),
    TextureSourceProfile(TEXT("texture_cube"), TEXT("Texture Cube"), TEXT("/Script/Engine.TextureCube")),
    Profile(TEXT("texture_cube_array"), TEXT("Texture Cube Array"),
            TEXT("/Script/Engine.TextureCubeArray")),
    Profile(TEXT("texture_graph"), TEXT("Texture Graph"),
            TEXT("/Script/TextureGraph.TextureGraph")),
    TextureSourceProfile(TEXT("volume_texture"), TEXT("Volume Texture"),
            TEXT("/Script/Engine.VolumeTexture")),
    Profile(TEXT("material_instance"), TEXT("Material Instance"),
            TEXT("/Script/Engine.MaterialInstanceConstant")),
};

bool RegisterIdentity(const FProfile &Profile, UE::UAI::Contract::FDiagnostic& OutError) {
  UeAgentAuthoringIdentity::FModelDescriptor Model;
  Model.ModelId = Profile.IdentityModelId;
  Model.TargetKind = Profile.TargetKind;
  Model.Selectors = {{TEXT("asset_path"), 100, false}};
  if (Profile.bProfileStateAsProperty || Profile.ProfileStateInputType == TEXT("json_array") || !Profile.GenerationInputSemanticId.IsEmpty())
    Model.Selectors.Add({TEXT("selector"), 0, true});
  Model.KeyPatterns = {{TEXT("asset"), 100, {TEXT("asset_path")}}};
  UeAgentAuthoringIdentity::ERegistrationResult Result;
  return UeAgentAuthoringIdentity::FRegistry::Get().RegisterModel(Model, Result,
                                                                  OutError);
}

bool RegisterSemantic(
    const FProfile &Profile, const FString &SemanticId,
    const UeAgentAuthoringSemanticRegistry::EWriteCapability WriteCapability,
    const bool bFactoryDefault, UE::UAI::Contract::FDiagnostic& OutError) {
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
  Semantic.Comparator = Profile.TypeId == TEXT("sequence") && SemanticId == Profile.ProfileStateSemanticId
      ? TEXT("uai.compare.movie_scene") : TEXT("uai.compare.canonical_value");
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

bool RegisterDescriptors(UE::UAI::Contract::FDiagnostic& OutError) {
  for (const FProfile &Item : Profiles) {
    if (!RegisterIdentity(Item, OutError))
      return false;
  }

  UeAgentAuthoringSemanticRegistry::FDomainDescriptor Domain;
  Domain.DomainId = DomainId;
  Domain.MutationProviderId = ApplyProviderId;
  for (const FProfile &Item : Profiles)
    Domain.TargetKinds.Add(Item.TargetKind);
  Domain.TargetKinds.Append({TEXT("behavior_tree.graph"), TEXT("behavior_tree.node"),
                            TEXT("behavior_tree.node_property"), TEXT("behavior_tree.edge")});
  Domain.TargetKinds.Append({TEXT("eqs.graph"), TEXT("eqs.node"),
                            TEXT("eqs.node_property"), TEXT("eqs.edge")});
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
                           EWrite::DomainMutation, false, OutError)) ||
        (!Item.GenerationInputSemanticId.IsEmpty() &&
         (!RegisterSemantic(Item, Item.GenerationInputSemanticId, EWrite::DomainMutation, false, OutError) ||
          !RegisterSemantic(Item, Item.GeneratedStateSemanticId, EWrite::ReadOnly, false, OutError)))) {
      return false;
    }
  }
  return UeAgentBehaviorTreeAuthoring::RegisterGraphState(OutError) &&
         UeAgentBlackboardAuthoring::RegisterBlackboardState(OutError) &&
         UeAgentEqsAuthoring::RegisterGraphState(OutError) &&
         UeAgentStateTreeAuthoring::RegisterTreeState(OutError) &&
         UeAgentSmartObjectAuthoring::RegisterDefinitionState(OutError);
}
} // namespace UeAgentReflectionAuthoring
