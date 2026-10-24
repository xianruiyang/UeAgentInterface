// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringState.h"
#include "Domains/Reflection/Authoring/UeAgentPaperAtlasAuthoringState.h"
#include "Domains/Material/Authoring/UeAgentMaterialCollectionAuthoring.h"
#include "Domains/PCG/Authoring/UeAgentPcgDataAssetAuthoring.h"
#include "PCGDataAsset.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/ScopeExit.h"
#include "LevelSequence.h"
#include "DomainAdapters/TimelineAnimation/UeAgentTimelineAnimationNative.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "Domains/Blackboard/Authoring/UeAgentBlackboardAuthoringState.h"
#include "Domains/BehaviorTree/Authoring/UeAgentBehaviorTreeAuthoringState.h"
#include "Domains/Eqs/Authoring/UeAgentEqsAuthoringState.h"
#include "Domains/StateTree/Authoring/UeAgentStateTreeAuthoringState.h"
#include "Domains/SmartObject/Authoring/UeAgentSmartObjectAuthoringState.h"
#include "SmartObjectDefinition.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringAssets.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringDescriptors.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringProfileState.h"
#include "Domains/Reflection/Authoring/UeAgentStaticMeshAuthoringState.h"
#include "Domains/Reflection/Authoring/UeAgentPhysicsAssetAuthoringState.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringGenerationInput.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringNativeDefaults.h"
#include "Utilities/UeAgentCurveJson.h"

namespace UeAgentReflectionAuthoring {
namespace {
// Only operation snapshots request this; public export retains its ordinary structure.
const FString SequenceMetadataContext = TEXT("sequence_binding_metadata_snapshot");
const FString PcgDataPreservationContext = TEXT("pcg_data_preservation_snapshot");
bool BuildIdentity(const FProfile &Profile, const FString &AssetPath, const FString &SemanticId,
                   UeAgentAuthoringIdentity::FCanonicalIdentity &OutIdentity,
                   UE::UAI::Contract::FDiagnostic& OutError) {
  TMap<FString, FString> Selectors{{TEXT("asset_path"), AssetPath}};
  // A whole-array state is one public property. Its declared business field
  // names that property through the existing composition selector seam.
  if (SemanticId == Profile.ProfileStateSemanticId && (Profile.bProfileStateAsProperty || Profile.ProfileStateInputType == TEXT("json_array")))
    Selectors.Add(TEXT("selector"), Profile.ProfileStateField);
  if (!Profile.GenerationInputSemanticId.IsEmpty() && SemanticId == Profile.GenerationInputSemanticId)
    Selectors.Add(TEXT("selector"), Profile.GenerationInputField);
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
  const FProfile *Profile =
      FindProfileByTargetKind(Request.TargetIdentity.TargetKind);
  if (!Profile) {
    OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("reflection_state_profile_missing"),
        TEXT("No Reflection state profile is registered for this target kind."), TEXT("state_capture"));
    OutError.SourceKind = TEXT("native_result");
    OutError.TargetPath = Request.TargetIdentity.GetSelector(TEXT("asset_path"));
    OutError.PropertyPath = TEXT("target_kind");
    OutError.ActualValue = MakeShared<FJsonValueString>(Request.TargetIdentity.TargetKind);
    OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("a registered Reflection target kind"));
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
  if (!Asset) {
    Asset = LoadAsset(*Profile,
                      Request.TargetIdentity.GetSelector(TEXT("asset_path")), OutError);
    if (OutError.IsValid()) return UeAgentAuthoringState::EReadOutcome::Failed;
  }
  if (!Asset) {
    OutValue.bAvailable = false;
    OutValue.AvailabilityReason = TEXT("target_not_found");
    return UeAgentAuthoringState::EReadOutcome::Unavailable;
  }
  ON_SCOPE_EXIT { ProfileState::BindTarget(OutError, Asset->GetPathName(), Request.TargetIdentity.GetSelector(TEXT("asset_path"))); };
  TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
  if (!Profile->GenerationInputSemanticId.IsEmpty() &&
      (Request.SemanticId == Profile->GenerationInputSemanticId || Request.SemanticId == Profile->GeneratedStateSemanticId)) {
    const bool bInput = Request.SemanticId == Profile->GenerationInputSemanticId;
    if (!(bInput ? GenerationInput::ReadTemplate(*Profile, OutValue.Value, OutError)
                 : GenerationInput::ReadGeneratedState(*Profile, *Asset, OutValue.Value, OutError)))
      return UeAgentAuthoringState::EReadOutcome::Failed;
    OutValue.bEditable = bInput;
    OutValue.bAuthoringVisible = bInput;
    OutValue.bAvailable = true;
    return UeAgentAuthoringState::EReadOutcome::Resolved;
  }
  if (Request.SemanticId == Profile->ExportTargetSemanticId) {
    Value->SetStringField(
        TEXT("asset_path"),
        Request.TargetIdentity.GetSelector(TEXT("asset_path")));
    Value->SetStringField(TEXT("asset_kind"), Profile->TypeId);
    Value->SetStringField(TEXT("class_path"), Asset->GetClass()->GetPathName());
    if (const FString* AtlasMode = Request.Context.Find(TEXT("paper_atlas_snapshot"))) {
      const auto* AtlasState = static_cast<const PaperSpriteState::FAtlasState*>(Request.RootData);
      if (!AtlasState) {
        OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("paper_atlas_snapshot_state_missing"),
            TEXT("The atlas snapshot request has no captured atlas state."), TEXT("state_capture"));
        OutError.SourceKind = TEXT("native_result");
        OutError.PropertyPath = TEXT("atlas_state");
        OutError.ActualValuePresent = false;
        OutError.ExpectedValue = MakeShared<FJsonValueString>(*AtlasMode);
        return UeAgentAuthoringState::EReadOutcome::Failed;
      }
      Value->SetField(TEXT("atlas_state"), AtlasState->Capture(*AtlasMode == TEXT("expected")));
    }
    if (const auto* Sequence = Cast<ULevelSequence>(Asset))
      Value->SetObjectField(TEXT("binding_resolvers"),
          UE::UAI::DomainAdapters::TimelineAnimation::ReadLevelSequenceBindingResolverSummary(*Sequence));
  } else if (!Profile->ProfileStateSemanticId.IsEmpty() &&
             Request.SemanticId == Profile->ProfileStateSemanticId) {
    TSharedPtr<FJsonValue> ProfileStateValue;
    bool bAuthoringVisible = true;
    if (!ProfileState::Read(
            *Profile, *Asset,
            Request.TargetIdentity.GetSelector(TEXT("asset_path")),
            ProfileStateValue, bAuthoringVisible, OutError)) {
      return UeAgentAuthoringState::EReadOutcome::Failed;
    }
    if (!ProfileStateValue.IsValid()) {
      OutValue.bAvailable = false;
      OutValue.AvailabilityReason = TEXT("profile_state_absent");
      return UeAgentAuthoringState::EReadOutcome::Unavailable;
    }
    if (Request.Context.FindRef(SequenceMetadataContext) == TEXT("true")) {
      const ULevelSequence* Sequence = Cast<ULevelSequence>(Asset);
      TSharedPtr<FJsonObject> Metadata;
      UE::UAI::DomainAdapters::TimelineAnimation::FUeAgentTimelineAnimationExecutionError Error;
      if (!Sequence) {
        OutError = ProfileState::TargetFailure(TEXT("sequence_metadata_snapshot_invalid"), *Asset, *ULevelSequence::StaticClass(), TEXT("state_capture"));
        return UeAgentAuthoringState::EReadOutcome::Failed;
      }
      if (!UE::UAI::DomainAdapters::TimelineAnimation::TryReadLevelSequenceBindingMetadataState(*Sequence, Metadata, Error)) {
        OutError = Error;
        return UeAgentAuthoringState::EReadOutcome::Failed;
      }
      auto InternalState = MakeShared<FJsonObject>();
      InternalState->SetField(TEXT("structure"), ProfileStateValue);
      InternalState->SetObjectField(TEXT("binding_metadata"), Metadata);
      ProfileStateValue = MakeShared<FJsonValueObject>(InternalState);
      bAuthoringVisible = false;
    }
    if (Request.Context.Contains(PcgDataPreservationContext)) {
      auto* Data = Cast<UPCGDataAsset>(Asset);
      if (!Data) { OutError = ProfileState::TargetFailure(TEXT("pcg_data_target_invalid"), *Asset, *UPCGDataAsset::StaticClass(), TEXT("state_capture")); return UeAgentAuthoringState::EReadOutcome::Failed; }
      UeAgentPcgDataAssetAuthoring::AppendPreservationSnapshot(*Data, ProfileStateValue);
      bAuthoringVisible = false;
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
    if (Profile->TypeId == TEXT("pcg_data_asset"))
      OutValue.ValueDefinition = MakeShared<FJsonValueObject>(UeAgentPcgDataAssetAuthoring::BuildValueDefinition());
    if (Profile->TypeId == TEXT("material_parameter_collection"))
      OutValue.ValueDefinition = MakeShared<FJsonValueObject>(UeAgentMaterialAuthoring::Collection::BuildValueDefinition());
    if (Profile->TypeId == TEXT("physics_asset"))
      OutValue.ValueDefinition = MakeShared<FJsonValueObject>(PhysicsAssetState::BuildValueDefinition());
    if (Profile->TypeId == TEXT("static_mesh"))
      OutValue.ValueDefinition = MakeShared<FJsonValueObject>(StaticMeshState::BuildValueDefinition());
    if (const auto* Contract = UeAgentCurveJson::FindAssetCurveContract(Profile->AssetClassPath))
      OutValue.ValueDefinition = MakeShared<FJsonValueObject>(UeAgentCurveJson::BuildCurveAssetValueDefinition(*Contract));
    OutValue.bEditable = !Request.Context.Contains(SequenceMetadataContext) && !Request.Context.Contains(PcgDataPreservationContext);
    OutValue.bAuthoringVisible = bAuthoringVisible;
    OutValue.bAvailable = true;
    return UeAgentAuthoringState::EReadOutcome::Resolved;
  } else if (Request.SemanticId != Profile->SeedSemanticId) {
    OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("reflection_state_semantic_unsupported"),
        TEXT("The requested state semantic is not supported by this Reflection profile."), TEXT("state_capture"));
    OutError.SourceKind = TEXT("native_result");
    OutError.PropertyPath = TEXT("semantic_id");
    OutError.ActualValue = MakeShared<FJsonValueString>(Request.SemanticId);
    TArray<TSharedPtr<FJsonValue>> Supported;
    for (const auto& Id : {Profile->ExportTargetSemanticId, Profile->ProfileStateSemanticId, Profile->SeedSemanticId,
        Profile->GenerationInputSemanticId, Profile->GeneratedStateSemanticId})
      if (!Id.IsEmpty()) Supported.Add(MakeShared<FJsonValueString>(Id));
    OutError.ExpectedValue = MakeShared<FJsonValueArray>(MoveTemp(Supported));
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
                   UE::UAI::Contract::FDiagnostic& OutError) {
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
    UE::UAI::Contract::FDiagnostic& OutError) {
  if (CanonicalAssetPath.IsEmpty()) {
    OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("reflection_snapshot_identity_invalid"),
        TEXT("The snapshot request requires a non-empty canonical asset path."), TEXT("state_capture"));
    OutError.SourceKind = TEXT("native_result");
    OutError.PropertyPath = TEXT("asset_path");
    OutError.ActualValue = MakeShared<FJsonValueString>(CanonicalAssetPath);
    OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("a non-empty canonical asset path"));
    return false;
  }
  if (!AddRequest(Profile, Profile.ExportTargetSemanticId, CanonicalAssetPath,
                  Asset, bExpectedAbsent, OutRequests, OutError))
    return false;
  if (!Profile.GeneratedStateSemanticId.IsEmpty() &&
      !AddRequest(Profile, Profile.GeneratedStateSemanticId, CanonicalAssetPath,
                  Asset, bExpectedAbsent, OutRequests, OutError)) return false;
  if (Profile.ProfileStateSemanticId.IsEmpty()) return true;
  if (!AddRequest(Profile, Profile.ProfileStateSemanticId, CanonicalAssetPath,
                  Asset, bExpectedAbsent, OutRequests, OutError)) return false;
  if (Profile.TypeId == TEXT("sequence"))
    OutRequests.Last().Context.Add(SequenceMetadataContext, TEXT("true"));
  if (Profile.TypeId == TEXT("pcg_data_asset"))
    OutRequests.Last().Context.Add(PcgDataPreservationContext, TEXT("true"));
  return true;
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
  if (!Profile.GenerationInputSemanticId.IsEmpty() &&
      (!AddRequest(Profile, Profile.GenerationInputSemanticId, AssetPath, &Asset, false, OutRequests, OutError) ||
       !AddRequest(Profile, Profile.GeneratedStateSemanticId, AssetPath, &Asset, false, OutRequests, OutError))) return false;
  if (Profile.TypeId == TEXT("behavior_tree")) {
    UBehaviorTree* Tree = Cast<UBehaviorTree>(&Asset);
    if (!Tree) { OutError = ProfileState::TargetFailure(TEXT("behavior_tree_export_target_invalid"), Asset, *UBehaviorTree::StaticClass(), TEXT("state_capture")); return false; }
    return UeAgentBehaviorTreeAuthoring::AppendGraphExportRequests(*Tree, OutRequests, OutError);
  }
  if (Profile.TypeId == TEXT("blackboard")) {
    const auto* Blackboard = Cast<UBlackboardData>(&Asset);
    if (!Blackboard) { OutError = ProfileState::TargetFailure(TEXT("blackboard_export_target_invalid"), Asset, *UBlackboardData::StaticClass(), TEXT("state_capture")); return false; }
    return UeAgentBlackboardAuthoring::AppendStructuralReadRequests(
        *Blackboard, AssetPath, OutRequests, OutError);
  }
  if (Profile.TypeId == TEXT("eqs")) {
    UEnvQuery* Query = Cast<UEnvQuery>(&Asset);
    if (!Query) { OutError = ProfileState::TargetFailure(TEXT("eqs_export_target_invalid"), Asset, *UEnvQuery::StaticClass(), TEXT("state_capture")); return false; }
    return UeAgentEqsAuthoring::AppendGraphExportRequests(*Query, OutRequests, OutError);
  }
  if (Profile.TypeId == TEXT("state_tree")) {
    const auto* Tree = Cast<UStateTree>(&Asset);
    if (!Tree) { OutError = ProfileState::TargetFailure(TEXT("state_tree_export_target_invalid"), Asset, *UStateTree::StaticClass(), TEXT("state_capture")); return false; }
    const auto* Data = Cast<UStateTreeEditorData>(Tree->EditorData);
    return !Data || UeAgentStateTreeAuthoring::AppendStructuralReadRequests(*Data, AssetPath, OutRequests, OutError);
  }
  if (Profile.TypeId == TEXT("smart_object")) {
    const auto* Definition = Cast<USmartObjectDefinition>(&Asset);
    if (!Definition) { OutError = ProfileState::TargetFailure(TEXT("smart_object_export_target_invalid"), Asset, *USmartObjectDefinition::StaticClass(), TEXT("state_capture")); return false; }
    return UeAgentSmartObjectAuthoring::AppendStructuralReadRequests(
        *Definition, AssetPath, OutRequests, OutError);
  }
  if (!Profile.ProfileStateSemanticId.IsEmpty() &&
      !AddRequest(Profile, Profile.ProfileStateSemanticId, AssetPath, &Asset,
                  false, OutRequests, OutError))
    return false;
  UeAgentAuthoringState::FEditablePropertyReadOptions Options;
  Options.SemanticId = TEXT("core.property");
  Options.ExcludedProperties = Profile.ExcludedRootProperties;
  if (Profile.IncludeProperty)
    Options.IncludeProperty = [Profile](const FProperty* Property, const void*, const UObject*) { return Profile.IncludeProperty(Property); };
  Options.Limits.AuthorizeProperty = [Profile](const FProperty* Property) { return Profile.AuthorizesProperty(Property); };
  return UeAgentAuthoringState::AppendEditableObjectPropertyReadRequests(
      &Asset, Options, OutRequests, OutError);
}

bool RegisterStateAccessor(UE::UAI::Contract::FDiagnostic& OutError) {
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

bool RegisterNativeDefaultProvider(UE::UAI::Contract::FDiagnostic& OutError) {
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
