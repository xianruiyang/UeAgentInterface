// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringState.h"

#include "Dom/JsonObject.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringAssets.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringDescriptors.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringProfileState.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringNativeDefaults.h"

namespace UeAgentReflectionAuthoring {
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
  const FProfile *Profile =
      FindProfileByTargetKind(Request.TargetIdentity.TargetKind);
  if (!Profile) {
    OutError = TEXT("reflection_state_profile_missing:") +
               Request.TargetIdentity.TargetKind;
    return UeAgentAuthoringState::EReadOutcome::Failed;
  }
  if (!Profile->ProfileStateSemanticId.IsEmpty() &&
      Request.SemanticId == Profile->ProfileStateSemanticId &&
      Profile->ProfileStateInputType == TEXT("json_array")) {
    OutValue.Type.Kind = TEXT("array");
  }
  if (Request.Context.FindRef(TEXT("expected_absent"))
          .Equals(TEXT("true"), ESearchCase::IgnoreCase)) {
    OutValue.bAvailable = false;
    OutValue.AvailabilityReason = TEXT("target_not_found");
    return UeAgentAuthoringState::EReadOutcome::Unavailable;
  }
  UObject *Asset = const_cast<UObject *>(Request.RootObject);
  if (!Asset)
    Asset = LoadAsset(*Profile,
                      Request.TargetIdentity.GetSelector(TEXT("asset_path")));
  if (!Asset) {
    OutValue.bAvailable = false;
    OutValue.AvailabilityReason = TEXT("target_not_found");
    return UeAgentAuthoringState::EReadOutcome::Unavailable;
  }
  TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
  if (Request.SemanticId == Profile->ExportTargetSemanticId) {
    Value->SetStringField(
        TEXT("asset_path"),
        Request.TargetIdentity.GetSelector(TEXT("asset_path")));
    Value->SetStringField(TEXT("asset_kind"), Profile->TypeId);
    Value->SetStringField(TEXT("class_path"), Asset->GetClass()->GetPathName());
  } else if (!Profile->ProfileStateSemanticId.IsEmpty() &&
             Request.SemanticId == Profile->ProfileStateSemanticId) {
    TSharedPtr<FJsonValue> ProfileStateValue;
    if (!ProfileState::Read(*Profile, *Asset, ProfileStateValue, OutError)) {
      if (OutError.IsEmpty())
        OutError =
            TEXT("reflection_profile_state_read_failed:") + Profile->TypeId;
      return UeAgentAuthoringState::EReadOutcome::Failed;
    }
    if (!ProfileStateValue.IsValid()) {
      OutValue.bAvailable = false;
      OutValue.AvailabilityReason = TEXT("profile_state_absent");
      return UeAgentAuthoringState::EReadOutcome::Unavailable;
    }
    switch (ProfileStateValue->Type) {
    case EJson::Array:
      OutValue.Type.Kind = TEXT("array");
      break;
    case EJson::Object:
      OutValue.Type.Kind = TEXT("object");
      break;
    case EJson::String:
      OutValue.Type.Kind = TEXT("string");
      break;
    case EJson::Number:
      OutValue.Type.Kind = TEXT("number");
      break;
    case EJson::Boolean:
      OutValue.Type.Kind = TEXT("bool");
      break;
    case EJson::Null:
      OutValue.Type.Kind = TEXT("null");
      break;
    default:
      OutValue.Type.Kind = TEXT("*");
      break;
    }
    OutValue.Value = MoveTemp(ProfileStateValue);
    OutValue.bEditable = true;
    OutValue.bAvailable = true;
    return UeAgentAuthoringState::EReadOutcome::Resolved;
  } else if (Request.SemanticId != Profile->SeedSemanticId) {
    OutError =
        TEXT("reflection_state_semantic_unsupported:") + Request.SemanticId;
    return UeAgentAuthoringState::EReadOutcome::Failed;
  }
  OutValue.Value = MakeShared<FJsonValueObject>(Value);
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
  if (!Profile || !Profile->bAllowCreate)
    return UeAgentAuthoringNativeDefaults::EProbeOutcome::Unavailable;
  UPackage *Package = Scratch.CreateScratchPackage(OutError);
  if (!Package)
    return UeAgentAuthoringNativeDefaults::EProbeOutcome::Failed;
  if (!CreateFactorySeed(*Profile, Package, FName(TEXT("Seed")),
                         RF_Transactional, OutError)) {
    return UeAgentAuthoringNativeDefaults::EProbeOutcome::Failed;
  }
  OutValue.Value = MakeShared<FJsonValueObject>(MakeShared<FJsonObject>());
  OutValue.Provenance = FactoryDefaultProviderId;
  return UeAgentAuthoringNativeDefaults::EProbeOutcome::Resolved;
}
} // namespace

bool BuildAssetSnapshotReadRequests(
    const FProfile &Profile, UObject *Asset, const FString &CanonicalAssetPath,
    const bool bExpectedAbsent,
    TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
    FString &OutError) {
  if (CanonicalAssetPath.IsEmpty()) {
    OutError = TEXT("reflection_snapshot_identity_invalid");
    return false;
  }
  if (!AddRequest(Profile, Profile.ExportTargetSemanticId, CanonicalAssetPath,
                  Asset, bExpectedAbsent, OutRequests, OutError))
    return false;
  return Profile.ProfileStateSemanticId.IsEmpty() ||
         AddRequest(Profile, Profile.ProfileStateSemanticId, CanonicalAssetPath,
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
  if (!Profile.ProfileStateSemanticId.IsEmpty() &&
      !AddRequest(Profile, Profile.ProfileStateSemanticId, AssetPath, &Asset,
                  false, OutRequests, OutError))
    return false;
  UeAgentAuthoringState::FEditablePropertyReadOptions Options;
  Options.SemanticId = TEXT("core.property");
  Options.ExcludedProperties = Profile.ExcludedRootProperties;
  return UeAgentAuthoringState::AppendEditableObjectPropertyReadRequests(
      &Asset, Options, OutRequests, OutError);
}

bool RegisterStateAccessor(FString &OutError) {
  UeAgentAuthoringState::FStateAccessorDescriptor Descriptor;
  Descriptor.AccessorId = StateAccessorId;
  for (const FProfile &Profile : GetProfiles())
    Descriptor.TargetKinds.Add(Profile.TargetKind);
  Descriptor.CapabilityTags = {TEXT("reflection"), TEXT("asset"),
                               TEXT("recursive_object")};
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
  Descriptor.CapabilityTags = {TEXT("factory"), TEXT("cdo"), TEXT("asset"),
                               TEXT("reflection")};
  Descriptor.bRequiresScratch = true;
  UeAgentAuthoringNativeDefaults::ERegistrationResult Result;
  return UeAgentAuthoringNativeDefaults::FProviderRegistry::Get()
      .RegisterProvider(Descriptor, ProbeNativeDefault, Result, OutError);
}
} // namespace UeAgentReflectionAuthoring
