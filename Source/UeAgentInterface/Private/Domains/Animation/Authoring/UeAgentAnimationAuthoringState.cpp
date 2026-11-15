// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringState.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Domains/Animation/Authoring/UeAgentAnimationAuthoringDocuments.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringState.h"
#include "Animation/AnimBlueprint.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringNativeDefaults.h"
#include "UObject/Object.h"
#include "UObject/Package.h"

namespace UeAgentAnimationAuthoring {
namespace {
bool BuildIdentity(const FProfile &Profile, const FString &AssetPath, const FString &SemanticId,
                   UeAgentAuthoringIdentity::FCanonicalIdentity &OutIdentity,
                   UE::UAI::Contract::FDiagnostic& OutError) {
  TMap<FString, FString> Selectors{{TEXT("asset_path"), AssetPath}};
  if ((Profile.Kind == EAssetKind::AnimSequence || Profile.Kind == EAssetKind::AnimMontage) && SemanticId == Profile.DocumentSemanticId)
    Selectors.Add(TEXT("selector"), TEXT("animation"));
  if (Profile.Kind == EAssetKind::Skeleton && SemanticId == Profile.DocumentSemanticId)
    Selectors.Add(TEXT("selector"), TEXT("skeleton"));
  return UeAgentAuthoringIdentity::BuildCanonicalIdentity(
      Profile.IdentityModelId, Profile.TargetKind,
      Selectors, OutIdentity, OutError);
}

bool AddRequest(const FProfile &Profile, const FString &SemanticId,
                const FString &AssetPath, const UObject *Asset,
                const bool bExpectedAbsent,
                TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
                UE::UAI::Contract::FDiagnostic& OutError) {
  UeAgentAuthoringState::FReadRequest Request;
  Request.SemanticId = SemanticId;
  if (!BuildIdentity(Profile, AssetPath, SemanticId, Request.TargetIdentity, OutError))
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
          UeAgentAuthoringState::FStateValue &OutValue, UE::UAI::Contract::FDiagnostic& OutError) {
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
    OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_state_profile_missing"),
        TEXT("No animation state profile is registered for the requested target kind."), TEXT("state_capture"), Request.TargetIdentity.TargetKind);
    OutError.SourceKind = TEXT("native_result");
    OutError.TargetPath = Request.TargetIdentity.GetSelector(TEXT("asset_path"));
    OutError.ActualValue = MakeShared<FJsonValueNull>();
    OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("registered animation state profile"));
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
  } else if (Profile->Kind == EAssetKind::AnimBlueprint &&
             Request.SemanticId == AnimBlueprintSettingsSemanticId) {
    TSharedPtr<FJsonObject> Document;
    if (!BuildAnimBlueprintSettingsDocument(*Asset, Document, OutError)) {
      if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Request.TargetIdentity.GetSelector(TEXT("asset_path"));
      return UeAgentAuthoringState::EReadOutcome::Failed;
    }
    OutValue.Value = MakeShared<FJsonValueObject>(Document);
    OutValue.bEditable = true;
  } else if (Request.SemanticId == Profile->DocumentSemanticId) {
    TSharedPtr<FJsonObject> Document;
    if (!BuildVerificationDocument(Profile->Kind, *Asset, Document, OutError,
                       Request.TargetIdentity.GetSelector(TEXT("asset_path"))))
      return UeAgentAuthoringState::EReadOutcome::Failed;
    OutValue.Value = MakeShared<FJsonValueObject>(Document);
    if (Profile->Kind == EAssetKind::AnimSequence || Profile->Kind == EAssetKind::BlendSpace ||
        Profile->Kind == EAssetKind::Skeleton || Profile->Kind == EAssetKind::AnimMontage || Profile->Kind == EAssetKind::PoseAsset)
      OutValue.bEditable = true;
    if (Profile->Kind == EAssetKind::AnimBlueprint) {
      const FString* Graphs = Request.Context.Find(UeAgentBlueprintAuthoring::ObservedGraphGuidsContext);
      if (Graphs) {
        OutValue.bAuthoringVisible = false;
        if (Request.bCaptureAuthoringProjection) {
          const FString Path = Request.TargetIdentity.GetSelector(TEXT("asset_path"));
          TArray<UeAgentAuthoringState::FReadRequest> Roots;
          if (!AddRequest(*Profile, Profile->ExportTargetSemanticId, Path, Asset, false, Roots, OutError)
              || !AddRequest(*Profile, AnimBlueprintSettingsSemanticId, Path, Asset, false, Roots, OutError)
              || !UeAgentBlueprintAuthoring::CaptureObservedGraphBodies(*CastChecked<UAnimBlueprint>(Asset),
                  Path, *Graphs, TEXT("ue.anim_blueprint.asset"), MoveTemp(Roots), OutValue, OutError, &Request.Context))
            return UeAgentAuthoringState::EReadOutcome::Failed;
        }
      }
    }
  } else {
    OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_state_semantic_unsupported"),
        TEXT("The requested semantic is not supported by this animation state profile."), TEXT("state_capture"), Request.SemanticId);
    OutError.SourceKind = TEXT("native_result");
    OutError.TargetPath = Request.TargetIdentity.GetSelector(TEXT("asset_path"));
    const auto Actual = MakeShared<FJsonObject>();
    Actual->SetBoolField(TEXT("semantic_supported"), false);
    const auto Expected = MakeShared<FJsonObject>();
    Expected->SetBoolField(TEXT("semantic_supported"), true);
    OutError.ActualValue = MakeShared<FJsonValueObject>(Actual);
    OutError.ExpectedValue = MakeShared<FJsonValueObject>(Expected);
    return UeAgentAuthoringState::EReadOutcome::Failed;
  }
  OutValue.bAvailable = true;
  return UeAgentAuthoringState::EReadOutcome::Resolved;
}

UeAgentAuthoringNativeDefaults::EProbeOutcome
ProbeNativeDefault(const UeAgentAuthoringNativeDefaults::FRequest &Request,
                   UeAgentAuthoringNativeDefaults::FScratchScope &Scratch,
                   UeAgentAuthoringNativeDefaults::FProbeValue &OutValue,
                   UE::UAI::Contract::FDiagnostic& OutError) {
  OutValue = UeAgentAuthoringNativeDefaults::FProbeValue();
  const FProfile *Profile =
      FindProfileByTargetKind(Request.TargetIdentity.TargetKind);
  if (!Profile)
    return UeAgentAuthoringNativeDefaults::EProbeOutcome::Unavailable;
  if (Profile->Kind == EAssetKind::AnimMontage ||
      Profile->Kind == EAssetKind::PoseAsset ||
      Profile->Kind == EAssetKind::BlendSpace)
    return UeAgentAuthoringNativeDefaults::EProbeOutcome::Unavailable;
  UPackage *Package = Scratch.CreateScratchPackage(OutError);
  if (!Package)
    return UeAgentAuthoringNativeDefaults::EProbeOutcome::Failed;
  UObject *Seed = CreateFactorySeed(Profile->Kind, Package, FName(TEXT("Seed")),
                                    RF_Transactional | RF_Transient, nullptr, OutError);
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
  case EAssetKind::AnimSequence:
  case EAssetKind::BlendSpace:
    return {};
  case EAssetKind::AnimMontage:
    return {TEXT("CompositeSections"), TEXT("SlotAnimTracks"), TEXT("Notifies"), TEXT("AnimNotifyTracks")};
  case EAssetKind::PoseAsset:
    return {TEXT("PoseContainer"), TEXT("bAdditivePose"), TEXT("BasePoseIndex")};
  default:
    return {};
  }
}
} // namespace

bool BuildAssetSnapshotReadRequests(
    const FProfile &Profile, UObject *Asset, const FString &CanonicalAssetPath,
    const bool bExpectedAbsent,
    TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
    UE::UAI::Contract::FDiagnostic& OutError) {
  if (CanonicalAssetPath.IsEmpty()) {
    OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_snapshot_identity_invalid"),
        TEXT("Animation snapshot capture requires a canonical asset path."), TEXT("state_capture"));
    OutError.SourceKind = TEXT("native_result");
    OutError.PropertyPath = TEXT("asset_path");
    OutError.ActualValue = MakeShared<FJsonValueString>(CanonicalAssetPath);
    OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty canonical asset path"));
    return false;
  }
  return AddRequest(Profile, Profile.DocumentSemanticId, CanonicalAssetPath,
                    Asset, bExpectedAbsent, OutRequests, OutError);
}

bool BuildAssetExportSnapshotReadRequests(
    const FProfile &Profile, UObject &Asset,
    TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
    UE::UAI::Contract::FDiagnostic& OutError) {
  OutRequests.Reset();
  OutError.Reset();
  const FString AssetPath = Asset.GetOutermost()->GetName();
  if (!AddRequest(Profile, Profile.ExportTargetSemanticId, AssetPath, &Asset,
                  false, OutRequests, OutError))
    return false;
  OutRequests.Last().MarkExportTarget();
  UeAgentAuthoringState::FEditablePropertyReadOptions Options;
  Options.SemanticId = TEXT("core.property");
  Options.ExcludedProperties = GetDomainOwnedRootProperties(Profile.Kind);
  if (!UeAgentAuthoringState::AppendEditableObjectPropertyReadRequests(
          &Asset, Options, OutRequests, OutError))
    return false;
  const FString DocumentSemantic = Profile.Kind == EAssetKind::AnimBlueprint
      ? FString(AnimBlueprintSettingsSemanticId) : Profile.DocumentSemanticId;
  return AddRequest(Profile, DocumentSemantic, AssetPath, &Asset,
                    false, OutRequests, OutError);
}

bool RegisterStateAccessor(UE::UAI::Contract::FDiagnostic& OutError) {
  UeAgentAuthoringState::FStateAccessorDescriptor Descriptor;
  Descriptor.AccessorId = StateAccessorId;
  for (const FProfile &Profile : GetProfiles())
    Descriptor.TargetKinds.Add(Profile.TargetKind);
  Descriptor.CapabilityTags = {TEXT("asset"),         TEXT("anim_blueprint"),
                               TEXT("graph"),         TEXT("skeleton"),
                               TEXT("socket"),        TEXT("virtual_bone"),
                               TEXT("skeletal_mesh"), TEXT("material_slot"),
                               TEXT("anim_sequence"), TEXT("blend_space")};
  UeAgentAuthoringState::ERegistrationResult Result;
  return UeAgentAuthoringState::FStateAccessorRegistry::Get().RegisterAccessor(
      Descriptor, ReadState, Result, OutError);
}

bool RegisterNativeDefaultProvider(UE::UAI::Contract::FDiagnostic& OutError) {
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
