// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Audio/Authoring/UeAgentAudioAuthoringDescriptors.h"

#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticRegistry.h"

namespace UeAgentAudioAuthoring {
namespace {
const TArray<FProfile> Profiles = {
    {
        EAssetKind::MetaSound,
        TEXT("metasound"),
        TEXT("metasound"),
        TEXT("audio.metasound"),
        TEXT("audio_metasound_identity"),
        TEXT("uai.semantic.audio.metasound.document"),
        TEXT("uai.semantic.audio.metasound.export.target"),
        TEXT("uai.apply_input.audio.metasound"),
        TEXT("uai.apply_workspace.audio.metasound"),
        TEXT("uai.export.audio.metasound"),
    },
    {
        EAssetKind::SoundCue,
        TEXT("sound_cue"),
        TEXT("sound_cue"),
        TEXT("audio.sound_cue"),
        TEXT("audio_sound_cue_identity"),
        TEXT("uai.semantic.audio.sound_cue.document"),
        TEXT("uai.semantic.audio.sound_cue.export.target"),
        TEXT("uai.apply_input.audio.sound_cue"),
        TEXT("uai.apply_workspace.audio.sound_cue"),
        TEXT("uai.export.audio.sound_cue"),
    },
    {
        EAssetKind::SoundSubmix,
        TEXT("sound_submix"),
        TEXT("sound_submix"),
        TEXT("audio.sound_submix"),
        TEXT("audio_sound_submix_identity"),
        TEXT("uai.semantic.audio.sound_submix.document"),
        TEXT("uai.semantic.audio.sound_submix.export.target"),
        TEXT("uai.apply_input.audio.sound_submix"),
        TEXT("uai.apply_workspace.audio.sound_submix"),
        TEXT("uai.export.audio.sound_submix"),
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
  Semantic.OwnerModel = TEXT("audio_asset_owner");
  Semantic.IdentityModel = Profile.IdentityModelId;
  Semantic.StateAccessor = StateAccessorId;
  Semantic.WriteCapability = WriteCapability;
  Semantic.ConstraintSource = TEXT("audio.asset.schema");
  Semantic.AvailabilitySource = StateAccessorId;
  Semantic.Comparator = TEXT("uai.compare.canonical_value");
  Semantic.CapabilityTags = {TEXT("audio"), TEXT("asset"), TEXT("graph"),
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
  case EAssetKind::MetaSound:
    return TEXT("metasound");
  case EAssetKind::SoundCue:
    return TEXT("sound_cue");
  case EAssetKind::SoundSubmix:
    return TEXT("sound_submix");
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
  Domain.CapabilityTags = {TEXT("asset_lifecycle"), TEXT("audio"),
                           TEXT("metasound"),       TEXT("sound_cue"),
                           TEXT("sound_submix"),    TEXT("graph")};
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
} // namespace UeAgentAudioAuthoring
