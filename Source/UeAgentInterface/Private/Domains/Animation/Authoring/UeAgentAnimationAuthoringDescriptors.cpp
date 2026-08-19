// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringDescriptors.h"

#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticRegistry.h"

namespace UeAgentAnimationAuthoring {
namespace {
const TArray<FProfile> Profiles = {
    {
        EAssetKind::AnimBlueprint,
        TEXT("anim_blueprint"),
        TEXT("anim_blueprint"),
        TEXT("animation.anim_blueprint"),
        TEXT("animation_anim_blueprint_identity"),
        TEXT("uai.semantic.animation.anim_blueprint.document"),
        TEXT("uai.semantic.animation.anim_blueprint.export.target"),
        TEXT("uai.apply_input.animation.anim_blueprint"),
        TEXT("uai.apply_workspace.animation.anim_blueprint"),
        TEXT("uai.export.animation.anim_blueprint"),
    },
    {
        EAssetKind::Skeleton,
        TEXT("skeleton"),
        TEXT("skeleton"),
        TEXT("animation.skeleton"),
        TEXT("animation_skeleton_identity"),
        TEXT("uai.semantic.animation.skeleton.document"),
        TEXT("uai.semantic.animation.skeleton.export.target"),
        TEXT("uai.apply_input.animation.skeleton"),
        TEXT("uai.apply_workspace.animation.skeleton"),
        TEXT("uai.export.animation.skeleton"),
    },
    {
        EAssetKind::SkeletalMesh,
        TEXT("skeletal_mesh"),
        TEXT("skeletal_mesh"),
        TEXT("animation.skeletal_mesh"),
        TEXT("animation_skeletal_mesh_identity"),
        TEXT("uai.semantic.animation.skeletal_mesh.document"),
        TEXT("uai.semantic.animation.skeletal_mesh.export.target"),
        TEXT("uai.apply_input.animation.skeletal_mesh"),
        TEXT("uai.apply_workspace.animation.skeletal_mesh"),
        TEXT("uai.export.animation.skeletal_mesh"),
    },
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
  Semantic.ValueType = TEXT("object");
  Semantic.OwnerModel = TEXT("animation_asset_owner");
  Semantic.IdentityModel = Profile.IdentityModelId;
  Semantic.StateAccessor = StateAccessorId;
  Semantic.WriteCapability = WriteCapability;
  Semantic.ConstraintSource = TEXT("animation.asset.schema");
  Semantic.AvailabilitySource = StateAccessorId;
  Semantic.Comparator = TEXT("uai.compare.canonical_value");
  Semantic.CapabilityTags = {TEXT("animation"), TEXT("asset"),
                             TEXT("reference_boundary"),
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

const FProfile *FindProfile(const EAssetKind Kind) {
  return Profiles.FindByPredicate(
      [Kind](const FProfile &Profile) { return Profile.Kind == Kind; });
}

const FProfile *FindProfileByTypeId(const FString &TypeId) {
  return Profiles.FindByPredicate([&TypeId](const FProfile &Profile) {
    return Profile.TypeId.Equals(TypeId, ESearchCase::IgnoreCase);
  });
}

const FProfile *FindProfileByTargetKind(const FString &TargetKind) {
  return Profiles.FindByPredicate([&TargetKind](const FProfile &Profile) {
    return Profile.TargetKind.Equals(TargetKind, ESearchCase::IgnoreCase);
  });
}

const TCHAR *AssetKindToString(const EAssetKind Kind) {
  switch (Kind) {
  case EAssetKind::AnimBlueprint:
    return TEXT("anim_blueprint");
  case EAssetKind::Skeleton:
    return TEXT("skeleton");
  case EAssetKind::SkeletalMesh:
    return TEXT("skeletal_mesh");
  default:
    return TEXT("unknown");
  }
}

bool ParseAssetKind(const FString &Text, EAssetKind &OutKind) {
  if (const FProfile *Profile = FindProfileByTypeId(Text)) {
    OutKind = Profile->Kind;
    return true;
  }
  return false;
}

bool RegisterDescriptors(FString &OutError) {
  for (const FProfile &Profile : Profiles) {
    if (!RegisterIdentity(Profile, OutError))
      return false;
  }

  UeAgentAuthoringSemanticRegistry::FDomainDescriptor Domain;
  Domain.DomainId = DomainId;
  Domain.MutationProviderId = ApplyProviderId;
  for (const FProfile &Profile : Profiles)
    Domain.TargetKinds.Add(Profile.TargetKind);
  Domain.CapabilityTags = {TEXT("asset_lifecycle"), TEXT("anim_blueprint"),
                           TEXT("graph"),           TEXT("skeleton"),
                           TEXT("socket"),          TEXT("virtual_bone"),
                           TEXT("skeletal_mesh"),   TEXT("material_slot")};
  UeAgentAuthoringSemanticRegistry::ERegistrationResult DomainResult;
  if (!UeAgentAuthoringSemanticRegistry::FRegistry::Get().RegisterDomain(
          Domain, DomainResult, OutError))
    return false;

  using EWrite = UeAgentAuthoringSemanticRegistry::EWriteCapability;
  for (const FProfile &Profile : Profiles) {
    if (!RegisterSemantic(Profile, Profile.DocumentSemanticId,
                          EWrite::DomainMutation, true, OutError) ||
        !RegisterSemantic(Profile, Profile.ExportTargetSemanticId,
                          EWrite::ReadOnly, false, OutError)) {
      return false;
    }
  }
  return true;
}
} // namespace UeAgentAnimationAuthoring
