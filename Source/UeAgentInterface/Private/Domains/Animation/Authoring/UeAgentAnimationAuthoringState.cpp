// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringState.h"

#include "Dom/JsonObject.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringDocuments.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringNativeDefaults.h"
#include "UObject/Object.h"
#include "UObject/Package.h"

namespace UeAgentAnimationAuthoring {
namespace {
bool BuildIdentity(const FProfile &Profile, const FString &AssetPath,
                   UeAgentAuthoringIdentity::FCanonicalIdentity &OutIdentity,
                   FString &OutError) {
  return UeAgentAuthoringIdentity::BuildCanonicalIdentity(
      Profile.IdentityModelId, Profile.TargetKind,
      {{TEXT("asset_path"), AssetPath}}, OutIdentity, OutError);
}

bool AddRequest(const FProfile &Profile, const FString &SemanticId,
                const FString &AssetPath, const UObject *Asset,
                const bool bExpectedAbsent,
                TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
                FString &OutError) {
  UeAgentAuthoringState::FReadRequest Request;
  Request.SemanticId = SemanticId;
  if (!BuildIdentity(Profile, AssetPath, Request.TargetIdentity, OutError))
    return false;
  Request.TargetQuery.ModelId = Request.TargetIdentity.ModelId;
  Request.TargetQuery.TargetKind = Request.TargetIdentity.TargetKind;
  Request.TargetQuery.CanonicalKey = Request.TargetIdentity.CanonicalKey;
  Request.RootObject = Asset;
  Request.RootOwnerPath = AssetPath;
  if (bExpectedAbsent)
    Request.Context.Add(TEXT("expected_absent"), TEXT("true"));
  OutRequests.Add(MoveTemp(Request));
  return true;
}

UeAgentAuthoringState::EReadOutcome
ReadState(const UeAgentAuthoringState::FReadRequest &Request,
          UeAgentAuthoringState::FStateValue &OutValue, FString &OutError) {
  OutValue = UeAgentAuthoringState::FStateValue();
  OutValue.TargetIdentity = Request.TargetIdentity;
  OutValue.Type.Kind = TEXT("object");
  OutValue.Provenance = StateAccessorId;
  OutError.Reset();
  if (Request.Context.FindRef(TEXT("expected_absent"))
          .Equals(TEXT("true"), ESearchCase::IgnoreCase)) {
    OutValue.bAvailable = false;
    OutValue.AvailabilityReason = TEXT("target_not_found");
    return UeAgentAuthoringState::EReadOutcome::Unavailable;
  }
  const FProfile *Profile =
      FindProfileByTargetKind(Request.TargetIdentity.TargetKind);
  if (!Profile) {
    OutError = TEXT("animation_state_profile_missing:") +
               Request.TargetIdentity.TargetKind;
    return UeAgentAuthoringState::EReadOutcome::Failed;
  }
  UObject *Asset = const_cast<UObject *>(Request.RootObject);
  if (!Asset)
    Asset = LoadAsset(Profile->Kind,
                      Request.TargetIdentity.GetSelector(TEXT("asset_path")));
  if (!Asset) {
    OutValue.bAvailable = false;
    OutValue.AvailabilityReason = TEXT("target_not_found");
    return UeAgentAuthoringState::EReadOutcome::Unavailable;
  }
  if (Request.SemanticId == Profile->ExportTargetSemanticId) {
    TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
    Target->SetStringField(TEXT("asset_path"),
                           Asset->GetOutermost()->GetName());
    Target->SetStringField(TEXT("asset_kind"), Profile->TypeId);
    OutValue.Value = MakeShared<FJsonValueObject>(Target);
  } else if (Request.SemanticId == Profile->DocumentSemanticId) {
    TSharedPtr<FJsonObject> Document;
    if (!BuildDocument(Profile->Kind, *Asset, Document, OutError,
                       Request.TargetIdentity.GetSelector(TEXT("asset_path"))))
      return UeAgentAuthoringState::EReadOutcome::Failed;
    OutValue.Value = MakeShared<FJsonValueObject>(Document);
  } else {
    OutError =
        TEXT("animation_state_semantic_unsupported:") + Request.SemanticId;
    return UeAgentAuthoringState::EReadOutcome::Failed;
  }
  OutValue.bAvailable = true;
  return UeAgentAuthoringState::EReadOutcome::Resolved;
}

UeAgentAuthoringNativeDefaults::EProbeOutcome
ProbeNativeDefault(const UeAgentAuthoringNativeDefaults::FRequest &Request,
                   UeAgentAuthoringNativeDefaults::FScratchScope &Scratch,
                   UeAgentAuthoringNativeDefaults::FProbeValue &OutValue,
                   FString &OutError) {
  OutValue = UeAgentAuthoringNativeDefaults::FProbeValue();
  const FProfile *Profile =
      FindProfileByTargetKind(Request.TargetIdentity.TargetKind);
  if (!Profile)
    return UeAgentAuthoringNativeDefaults::EProbeOutcome::Unavailable;
  UPackage *Package = Scratch.CreateScratchPackage(OutError);
  if (!Package)
    return UeAgentAuthoringNativeDefaults::EProbeOutcome::Failed;
  UObject *Seed = CreateFactorySeed(Profile->Kind, Package, FName(TEXT("Seed")),
                                    RF_Transactional, nullptr, OutError);
  if (!Seed)
    return UeAgentAuthoringNativeDefaults::EProbeOutcome::Failed;
  TSharedPtr<FJsonObject> Document;
  if (!BuildDocument(Profile->Kind, *Seed, Document, OutError))
    return UeAgentAuthoringNativeDefaults::EProbeOutcome::Failed;
  OutValue.Value = MakeShared<FJsonValueObject>(Document);
  OutValue.Provenance = FactoryDefaultProviderId;
  return UeAgentAuthoringNativeDefaults::EProbeOutcome::Resolved;
}

TSet<FName> GetDomainOwnedRootProperties(const EAssetKind Kind) {
  switch (Kind) {
  case EAssetKind::AnimBlueprint:
    return {TEXT("ParentClass"),
            TEXT("TargetSkeleton"),
            TEXT("bIsTemplate"),
            TEXT("PreviewSkeletalMesh"),
            TEXT("PreviewAnimationBlueprint"),
            TEXT("PreviewAnimationBlueprintApplicationMethod"),
            TEXT("PreviewAnimationBlueprintTag"),
            TEXT("bUseMultiThreadedAnimationUpdate"),
            TEXT("bWarnAboutBlueprintUsage")};
  case EAssetKind::Skeleton:
    return {TEXT("PreviewSkeletalMesh"),
            TEXT("CompatibleSkeletons"),
            TEXT("bUseRetargetModesFromCompatibleSkeleton"),
            TEXT("Sockets"),
            TEXT("VirtualBones"),
            TEXT("SlotGroups"),
            TEXT("BoneTree")};
  case EAssetKind::SkeletalMesh:
    return {TEXT("Skeleton"), TEXT("Materials"), TEXT("Sockets"),
            TEXT("PhysicsAsset"), TEXT("PostProcessAnimBlueprint")};
  default:
    return {};
  }
}
} // namespace

bool BuildAssetSnapshotReadRequests(
    const FProfile &Profile, UObject *Asset, const FString &CanonicalAssetPath,
    const bool bExpectedAbsent,
    TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
    FString &OutError) {
  if (CanonicalAssetPath.IsEmpty()) {
    OutError = TEXT("animation_snapshot_identity_invalid");
    return false;
  }
  return AddRequest(Profile, Profile.DocumentSemanticId, CanonicalAssetPath,
                    Asset, bExpectedAbsent, OutRequests, OutError);
}

bool BuildAssetExportSnapshotReadRequests(
    const FProfile &Profile, UObject &Asset,
    TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
    FString &OutError) {
  OutRequests.Reset();
  OutError.Reset();
  const FString AssetPath = Asset.GetOutermost()->GetName();
  if (!AddRequest(Profile, Profile.ExportTargetSemanticId, AssetPath, &Asset,
                  false, OutRequests, OutError))
    return false;
  UeAgentAuthoringState::FEditablePropertyReadOptions Options;
  Options.SemanticId = TEXT("core.property");
  Options.ExcludedProperties = GetDomainOwnedRootProperties(Profile.Kind);
  if (!UeAgentAuthoringState::AppendEditableObjectPropertyReadRequests(
          &Asset, Options, OutRequests, OutError))
    return false;
  return AddRequest(Profile, Profile.DocumentSemanticId, AssetPath, &Asset,
                    false, OutRequests, OutError);
}

bool RegisterStateAccessor(FString &OutError) {
  UeAgentAuthoringState::FStateAccessorDescriptor Descriptor;
  Descriptor.AccessorId = StateAccessorId;
  for (const FProfile &Profile : GetProfiles())
    Descriptor.TargetKinds.Add(Profile.TargetKind);
  Descriptor.CapabilityTags = {TEXT("asset"),         TEXT("anim_blueprint"),
                               TEXT("graph"),         TEXT("skeleton"),
                               TEXT("socket"),        TEXT("virtual_bone"),
                               TEXT("skeletal_mesh"), TEXT("material_slot")};
  UeAgentAuthoringState::ERegistrationResult Result;
  return UeAgentAuthoringState::FStateAccessorRegistry::Get().RegisterAccessor(
      Descriptor, ReadState, Result, OutError);
}

bool RegisterNativeDefaultProvider(FString &OutError) {
  UeAgentAuthoringNativeDefaults::FProviderDescriptor Descriptor;
  Descriptor.ProviderId = FactoryDefaultProviderId;
  Descriptor.SourceKind =
      UeAgentAuthoringSemanticRegistry::EDefaultSourceKind::Factory;
  for (const FProfile &Profile : GetProfiles())
    Descriptor.TargetKinds.Add(Profile.TargetKind);
  Descriptor.CapabilityTags = {TEXT("factory"), TEXT("asset"),
                               TEXT("animation")};
  Descriptor.bRequiresScratch = true;
  UeAgentAuthoringNativeDefaults::ERegistrationResult Result;
  return UeAgentAuthoringNativeDefaults::FProviderRegistry::Get()
      .RegisterProvider(Descriptor, ProbeNativeDefault, Result, OutError);
}
} // namespace UeAgentAnimationAuthoring
