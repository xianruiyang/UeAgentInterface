// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringDescriptors.h"

#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticRegistry.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticCompare.h"
#include "Domains/Animation/Authoring/UeAgentAnimationMontageStructure.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringDocuments.h"

namespace UeAgentAnimationAuthoring {
namespace {
const TArray<FProfile> Profiles = {
    {
        EAssetKind::PoseAsset,
        TEXT("pose_asset"), TEXT("pose_asset"), TEXT("animation.pose_asset"),
        TEXT("animation_pose_asset_identity"),
        TEXT("uai.semantic.animation.pose_asset.document"),
        TEXT("uai.semantic.animation.pose_asset.export.target"),
        TEXT("uai.apply_input.animation.pose_asset"),
        TEXT("uai.apply_workspace.animation.pose_asset"),
        TEXT("uai.export.animation.pose_asset"),
    },
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
    {
        EAssetKind::AnimSequence,
        TEXT("anim_sequence"),
        TEXT("anim_sequence"),
        TEXT("animation.anim_sequence"),
        TEXT("animation_anim_sequence_identity"),
        TEXT("uai.semantic.animation.anim_sequence.document"),
        TEXT("uai.semantic.animation.anim_sequence.export.target"),
        TEXT("uai.apply_input.animation.anim_sequence"),
        TEXT("uai.apply_workspace.animation.anim_sequence"),
        TEXT("uai.export.animation.anim_sequence"),
    },
    {
        EAssetKind::AnimMontage,
        TEXT("anim_montage"), TEXT("anim_montage"), TEXT("animation.anim_montage"),
        TEXT("animation_anim_montage_identity"),
        TEXT("uai.semantic.animation.anim_montage.document"),
        TEXT("uai.semantic.animation.anim_montage.export.target"),
        TEXT("uai.apply_input.animation.anim_montage"),
        TEXT("uai.apply_workspace.animation.anim_montage"),
        TEXT("uai.export.animation.anim_montage"),
    },
    {
        EAssetKind::BlendSpace,
        TEXT("blend_space"),
        TEXT("blend_space"),
        TEXT("animation.blend_space"),
        TEXT("animation_blend_space_identity"),
        TEXT("uai.semantic.animation.blend_space.document"),
        TEXT("uai.semantic.animation.blend_space.export.target"),
        TEXT("uai.apply_input.animation.blend_space"),
        TEXT("uai.apply_workspace.animation.blend_space"),
        TEXT("uai.export.animation.blend_space"),
    },
};

bool RegisterIdentity(const FProfile &Profile, UE::UAI::Contract::FDiagnostic& OutError) {
  UeAgentAuthoringIdentity::FModelDescriptor Model;
  Model.ModelId = Profile.IdentityModelId;
  Model.TargetKind = Profile.TargetKind;
  Model.Selectors = {{TEXT("asset_path"), 100, false}};
  if (Profile.Kind == EAssetKind::AnimSequence || Profile.Kind == EAssetKind::AnimMontage || Profile.Kind == EAssetKind::Skeleton)
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
  Semantic.ValueType = TEXT("object");
  Semantic.OwnerModel = TEXT("animation_asset_owner");
  Semantic.IdentityModel = Profile.IdentityModelId;
  Semantic.StateAccessor = StateAccessorId;
  Semantic.WriteCapability = WriteCapability;
  Semantic.ConstraintSource = TEXT("animation.asset.schema");
  Semantic.AvailabilitySource = StateAccessorId;
  Semantic.Comparator = Profile.Kind == EAssetKind::AnimMontage
      ? TEXT("uai.compare.animation_montage")
      : (Profile.Kind == EAssetKind::Skeleton || Profile.Kind == EAssetKind::SkeletalMesh)
          ? TEXT("uai.compare.animation_document") : TEXT("uai.compare.canonical_value");
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
  case EAssetKind::AnimSequence:
    return TEXT("anim_sequence");
  case EAssetKind::AnimMontage:
    return TEXT("anim_montage");
  case EAssetKind::BlendSpace:
    return TEXT("blend_space");
  case EAssetKind::PoseAsset:
    return TEXT("pose_asset");
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

bool RegisterDescriptors(UE::UAI::Contract::FDiagnostic& OutError) {
  auto& Comparators = UeAgentAuthoringSemanticCompare::FComparatorRegistry::Get();
  UeAgentAuthoringSemanticCompare::FComparator Canonical;
  if (!UeAgentAuthoringSemanticCompare::RegisterBuiltInComparators(Comparators, OutError) ||
      !Comparators.CopyComparator(TEXT("uai.compare.canonical_value"), Canonical)) return false;
  UeAgentAuthoringSemanticCompare::FComparatorDescriptor Comparator;
  Comparator.ComparatorId = TEXT("uai.compare.animation_montage");
  Comparator.ValueKinds = {TEXT("object")};
  Comparator.CapabilityTags = {TEXT("canonical_value"), TEXT("native_ordered_identity")};
  UeAgentAuthoringSemanticCompare::ERegistrationResult ComparatorResult;
  if (!Comparators.RegisterComparator(Comparator, MoveTemp(Canonical), ComparatorResult, OutError,
      [](UeAgentAuthoringState::FStateEntry& NativeEntry, TSharedPtr<FJsonValue>& Before,
         TSharedPtr<FJsonValue>& Desired, TSharedPtr<FJsonValue>& Actual,
         TMap<FString, FString>& SourcePaths, UE::UAI::Contract::FDiagnostic& Error) {
        return ProjectMontageOriginalIntent(NativeEntry, Before, Desired, Actual, SourcePaths, Error);
      })) return false;
  if (!Comparators.CopyComparator(TEXT("uai.compare.canonical_value"), Canonical)) return false;
  Comparator.ComparatorId = TEXT("uai.compare.animation_document");
  Comparator.CapabilityTags = {TEXT("canonical_value"), TEXT("native_document_arguments")};
  if (!Comparators.RegisterComparator(Comparator, MoveTemp(Canonical), ComparatorResult, OutError,
      [](const UeAgentAuthoringState::FStateEntry&, TSharedPtr<FJsonValue>&,
         TSharedPtr<FJsonValue>& Desired, TSharedPtr<FJsonValue>&,
         TMap<FString, FString>&, UE::UAI::Contract::FDiagnostic& Error) {
        return ProjectDocumentOriginalIntent(Desired, Error);
      })) return false;
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
                           TEXT("skeletal_mesh"),   TEXT("material_slot"),
                           TEXT("anim_sequence"),   TEXT("blend_space")};
  UeAgentAuthoringSemanticRegistry::ERegistrationResult DomainResult;
  if (!UeAgentAuthoringSemanticRegistry::FRegistry::Get().RegisterDomain(
          Domain, DomainResult, OutError))
    return false;

  using EWrite = UeAgentAuthoringSemanticRegistry::EWriteCapability;
  for (const FProfile &Profile : Profiles) {
    const bool bSupportsFactoryDefault =
        Profile.Kind != EAssetKind::AnimMontage &&
        Profile.Kind != EAssetKind::PoseAsset &&
        Profile.Kind != EAssetKind::BlendSpace;
    if (!RegisterSemantic(Profile, Profile.DocumentSemanticId,
                          EWrite::DomainMutation, bSupportsFactoryDefault,
                          OutError) ||
        !RegisterSemantic(Profile, Profile.ExportTargetSemanticId,
                          EWrite::ReadOnly, false, OutError)) {
      return false;
    }
    if (Profile.Kind == EAssetKind::AnimBlueprint &&
        !RegisterSemantic(Profile, AnimBlueprintSettingsSemanticId,
                          EWrite::DomainMutation, false, OutError))
      return false;
  }
  return true;
}
} // namespace UeAgentAnimationAuthoring
