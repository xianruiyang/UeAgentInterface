// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Level/Authoring/UeAgentLevelAuthoringState.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringAuthorability.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringDescriptors.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringSpatial.h"
#include "Domains/PCG/Authoring/UeAgentPcgComponentAuthoring.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/WorldSettings.h"
#include "PCGComponent.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/NoExportTypes.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringNativeDefaults.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTypeDescriptors.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "WorldPartition/DataLayer/DataLayerAsset.h"
#include "WorldPartition/DataLayer/DataLayerInstance.h"
#include "WorldPartition/DataLayer/WorldDataLayers.h"

namespace UeAgentLevelAuthoring {
namespace {
UE::UAI::Contract::FDiagnostic StateFailure(const TCHAR* Code, const FString& Reason,
    const FString& Field, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
    const FJsonObject* Input = nullptr) {
  auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("state_capture"));
  Error.PropertyPath = Field;
  Error.ActualValuePresent = Actual.IsValid();
  Error.ActualValue = MoveTemp(Actual);
  Error.ExpectedValue = MoveTemp(Expected);
  if (Input) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Input, Field);
  return Error;
}

TArray<FString> GetManagedActorIds(const AActor &Actor) {
  TArray<FString> Result;
  for (const FName &Tag : Actor.Tags) {
    if (IsManagedActorIdentityTag(Tag) && Tag != GetManagedActorMarkerTag())
      Result.Add(Tag.ToString().Mid(7));
  }
  return Result;
}

FString GuidText(const FGuid &Guid) {
  return Guid.IsValid() ? Guid.ToString(EGuidFormats::DigitsWithHyphensLower)
                        : FString();
}

bool ParseGuid(const FString &Text, FGuid &OutGuid) {
  OutGuid.Invalidate();
  return !Text.IsEmpty() && FGuid::Parse(Text, OutGuid) && OutGuid.IsValid();
}

bool BuildIdentity(const FString &ModelId, const FString &TargetKind,
                   const TMap<FString, FString> &Selectors,
                   UeAgentAuthoringIdentity::FCanonicalIdentity &OutIdentity,
                   UE::UAI::Contract::FDiagnostic& OutError) {
  return UeAgentAuthoringIdentity::BuildCanonicalIdentity(
      ModelId, TargetKind, Selectors, OutIdentity, OutError);
}

TMap<FString, FString> ActorSelectors(const AActor &Actor) {
  TMap<FString, FString> Result = {
      {TEXT("world_path"), GetWorldPath(*Actor.GetWorld())},
      {TEXT("actor_guid"), GuidText(Actor.GetActorGuid())},
      {TEXT("managed_id"), GetManagedActorId(Actor)},
      {TEXT("object_path"), Actor.GetPathName()},
      {TEXT("actor_class"), Actor.GetClass()->GetPathName()},
  };
  return Result;
}

TMap<FString, FString> ComponentSelectors(const UActorComponent &Component) {
  const AActor *Owner = Component.GetOwner();
  return {
      {TEXT("world_path"), Owner && Owner->GetWorld()
                               ? GetWorldPath(*Owner->GetWorld())
                               : FString()},
      {TEXT("owner_actor_guid"),
       Owner ? GuidText(Owner->GetActorGuid()) : FString()},
      {TEXT("owner_managed_id"), Owner ? GetManagedActorId(*Owner) : FString()},
      {TEXT("component_name"), Component.GetName()},
      {TEXT("component_class"), Component.GetClass()->GetPathName()},
  };
}

bool AddReadRequest(
    const FString &SemanticId,
    const UeAgentAuthoringIdentity::FCanonicalIdentity &Identity,
    const UObject *RootObject, const bool bExpectedAbsent,
    TArray<UeAgentAuthoringState::FReadRequest> &OutRequests) {
  UeAgentAuthoringState::FReadRequest Request;
  Request.SemanticId = SemanticId;
  Request.TargetIdentity = Identity;
  Request.TargetQuery.ModelId = Identity.ModelId;
  Request.TargetQuery.TargetKind = Identity.TargetKind;
  Request.TargetQuery.CanonicalKey = Identity.CanonicalKey;
  Request.RootObject = RootObject;
  Request.RootOwnerPath = Identity.GetSelector(TEXT("world_path"));
  if (bExpectedAbsent)
    Request.Context.Add(TEXT("expected_absent"), TEXT("true"));
  OutRequests.Add(MoveTemp(Request));
  return true;
}

bool AppendEditableProperties(
    const UObject *Object, const FString &SemanticId, const FString &ModelId,
    const FString &TargetKind, const TMap<FString, FString> &BaseSelectors,
    const TSet<FName> &Excluded,
    TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
    UE::UAI::Contract::FDiagnostic& OutError) {
  UeAgentAuthoringState::FEditablePropertyReadOptions Options;
  Options.SemanticId = SemanticId;
  Options.ExcludedProperties = Excluded;
  Options.IncludeProperty = [](const FProperty* Property, const void* Value, const UObject* Owner) {
    const auto* Actor = Cast<AActor>(Owner);
    const auto* ObjectProperty = CastField<FObjectPropertyBase>(Property);
    const auto* Component = Actor && ObjectProperty
        ? Cast<UActorComponent>(ObjectProperty->GetObjectPropertyValue(Value)) : nullptr;
    // Owned components already have their own state and properties in the hierarchy.
    return !Component || Component->GetOwner() != Actor;
  };
  Options.BuildSnapshotIdentity =
      [ModelId, TargetKind,
       BaseSelectors](const FString &PropertyPath,
                      UeAgentAuthoringIdentity::FCanonicalIdentity &OutIdentity,
                      UE::UAI::Contract::FDiagnostic& IdentityError) {
        TMap<FString, FString> Selectors = BaseSelectors;
        Selectors.Add(TEXT("property_path"), PropertyPath);
        return BuildIdentity(ModelId, TargetKind, Selectors, OutIdentity,
                             IdentityError);
      };
  return UeAgentAuthoringState::AppendEditableObjectPropertyReadRequests(
      Object, Options, OutRequests, OutError);
}

TSharedRef<FJsonObject> BuildWorldTarget(const UWorld &World) {
  TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
  Result->SetStringField(TEXT("world_path"), GetWorldPath(World));
  return Result;
}

TSharedRef<FJsonObject>
BuildActorTarget(const AActor &Actor,
                 const UeAgentAuthoringIdentity::FCanonicalIdentity &Identity) {
  TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
  UeAgentAuthoringIdentity::WriteSelectorsFlat(Identity, Result);
  Result->SetStringField(TEXT("actor_class"), Actor.GetClass()->GetPathName());
  return Result;
}

bool ReadTransformOverride(const TMap<FString, FString> *Context,
                           const TCHAR *Prefix, FTransform &OutTransform) {
  if (!Context)
    return false;
  const FString *Encoded = Context->Find(FString(Prefix) + TEXT("transform_value"));
  if (!Encoded)
    return false;
  TSharedPtr<FJsonObject> Value;
  if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(*Encoded), Value))
    return false;
  UE::UAI::Contract::FDiagnostic Error;
  return UeAgentPropertyValueCodec::DecodeStructValue(
      TBaseStructure<FTransform>::Get(), MakeShared<FJsonValueObject>(Value),
      &OutTransform, Error);
}

TSharedRef<FJsonObject>
BuildActorState(const AActor &Actor,
                const TMap<FString, FString> *Context = nullptr) {
  TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
  const bool bOmitGeneratedLabel =
      Context && Context->FindRef(TEXT("actor_label_omit")) == TEXT("true");
  if (!bOmitGeneratedLabel) {
    Result->SetStringField(
        TEXT("label"),
        Context && Context->Contains(TEXT("actor_label_override"))
            ? Context->FindRef(TEXT("actor_label_override"))
            // Native-default reads must not cache a generated display label on
            // a CDO/archetype: that would change the defaults used by serialization.
            : Actor.GetActorLabel(!Actor.IsTemplate()));
  }
  Result->SetStringField(
      TEXT("folder_path"),
      Context && Context->Contains(TEXT("actor_folder_override"))
          ? Context->FindRef(TEXT("actor_folder_override"))
          : GetActorFolderPathText(Actor));
  FTransform Transform = Actor.GetActorTransform();
  ReadTransformOverride(Context, TEXT("actor_"), Transform);
  TSharedRef<FJsonObject> TransformState =
      BuildTransformState(Transform, ETransformFrame::ActorWorld);
  if (Context) {
    if (Context->FindRef(TEXT("actor_transform_translation_omit")) ==
        TEXT("true")) {
      TransformState->RemoveField(TEXT("translation"));
    }
    if (Context->FindRef(TEXT("actor_transform_rotation_omit")) ==
        TEXT("true")) {
      TransformState->RemoveField(TEXT("rotation"));
    }
    if (Context->FindRef(TEXT("actor_transform_scale_omit")) == TEXT("true")) {
      TransformState->RemoveField(TEXT("scale"));
    }
  }
  Result->SetObjectField(TEXT("transform"), TransformState);
  TArray<TSharedPtr<FJsonValue>> Tags;
  for (const FName &Tag : Actor.Tags) {
    if (!IsManagedActorIdentityTag(Tag))
      Tags.Add(MakeShared<FJsonValueString>(Tag.ToString()));
  }
  Result->SetArrayField(TEXT("tags"), MoveTemp(Tags));
  const bool bAttachmentOverride =
      Context &&
      Context->FindRef(TEXT("actor_attachment_override")) == TEXT("true");
  const AActor *Parent = Actor.GetAttachParentActor();
  TSharedRef<FJsonObject> Attachment = MakeShared<FJsonObject>();
  Attachment->SetStringField(
      TEXT("parent_managed_id"),
      bAttachmentOverride
          ? Context->FindRef(TEXT("actor_parent_managed_id_override"))
          : (Parent ? GetManagedActorId(*Parent) : FString()));
  Attachment->SetStringField(
      TEXT("socket_name"),
      bAttachmentOverride
          ? Context->FindRef(TEXT("actor_socket_name_override"))
          : (Actor.GetAttachParentSocketName().IsNone()
                 ? FString()
                 : Actor.GetAttachParentSocketName().ToString()));
  Result->SetObjectField(TEXT("attachment"), Attachment);
  return Result;
}

TSharedRef<FJsonObject> BuildComponentTarget(
    const UActorComponent &Component,
    const UeAgentAuthoringIdentity::FCanonicalIdentity &Identity) {
  TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
  UeAgentAuthoringIdentity::WriteSelectorsFlat(Identity, Result);
  Result->SetStringField(TEXT("component_class"),
                         Component.GetClass()->GetPathName());
  return Result;
}

TSharedRef<FJsonObject>
BuildComponentState(const UActorComponent &Component,
                    const TMap<FString, FString> *Context = nullptr) {
  TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
  if (const auto Pcg = UeAgentPcgComponentAuthoring::Read(Component))
    Result->SetObjectField(TEXT("pcg"), Pcg);
  const USceneComponent *Scene = Cast<USceneComponent>(&Component);
  if (!Scene)
    return Result;
  TSharedRef<FJsonObject> Attachment = MakeShared<FJsonObject>();
  const bool bOverride =
      Context && Context->FindRef(TEXT("attachment_override")) == TEXT("true");
  const USceneComponent *Parent = Scene->GetAttachParent();
  Attachment->SetStringField(
      TEXT("parent_component"),
      bOverride && Context->Contains(TEXT("parent_component_override"))
          ? Context->FindRef(TEXT("parent_component_override"))
          : (Parent ? Parent->GetName() : FString()));
  Attachment->SetStringField(
      TEXT("socket_name"),
      bOverride && Context->Contains(TEXT("socket_name_override"))
          ? Context->FindRef(TEXT("socket_name_override"))
          : (Scene->GetAttachSocketName().IsNone()
                 ? FString()
                 : Scene->GetAttachSocketName().ToString()));
  FTransform Transform = Scene->GetRelativeTransform();
  ReadTransformOverride(Context, TEXT("component_"), Transform);
  Attachment->SetObjectField(
      TEXT("transform"),
      BuildTransformState(Transform, ETransformFrame::ComponentParentRelative));
  Result->SetObjectField(TEXT("attachment"), Attachment);
  return Result;
}

TSharedPtr<FJsonValue> BuildSpatialStateDefinition(bool bComponent) {
  const auto Field = [](const TCHAR* Kind, const TSharedPtr<FJsonObject>& Definition = nullptr) {
    const auto Result = MakeShared<FJsonObject>();
    const auto Type = MakeShared<FJsonObject>();
    Type->SetStringField(TEXT("kind"), Kind);
    Result->SetObjectField(TEXT("type"), Type);
    if (Definition) Result->SetObjectField(TEXT("value_definition"), Definition);
    return Result;
  };
  const auto Record = [](const TSharedPtr<FJsonObject>& Fields) {
    const auto Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("fields_complete"), false);
    Result->SetObjectField(TEXT("fields"), Fields);
    return Result;
  };
  const auto TransformFields = MakeShared<FJsonObject>();
  const auto StructField = [](const UScriptStruct* Struct) {
    const auto Result = MakeShared<FJsonObject>();
    Result->SetObjectField(TEXT("type"), UeAgentAuthoringTypeDescriptors::DescribeStruct(Struct).ToJsonObject());
    return Result;
  };
  TransformFields->SetObjectField(TEXT("translation"), StructField(TBaseStructure<FVector>::Get()));
  TransformFields->SetObjectField(TEXT("rotation"), StructField(TBaseStructure<FRotator>::Get()));
  TransformFields->SetObjectField(TEXT("scale"), StructField(TBaseStructure<FVector>::Get()));
  auto Fields = MakeShared<FJsonObject>();
  Fields->SetObjectField(TEXT("transform"), Field(TEXT("object"), Record(TransformFields)));
  if (bComponent) {
    const auto AttachmentFields = Fields;
    Fields = MakeShared<FJsonObject>();
    Fields->SetObjectField(TEXT("attachment"), Field(TEXT("object"), Record(AttachmentFields)));
  }
  return MakeShared<FJsonValueObject>(Record(Fields));
}

UeAgentAuthoringState::EReadOutcome
ReadState(const UeAgentAuthoringState::FReadRequest &Request,
          UeAgentAuthoringState::FStateValue &OutValue, UE::UAI::Contract::FDiagnostic& OutError) {
  OutValue = UeAgentAuthoringState::FStateValue();
  OutValue.TargetIdentity = Request.TargetIdentity;
  OutValue.Provenance = StateAccessorId;
  OutValue.Type.Kind = TEXT("object");
  if (Request.SemanticId == ActorStateSemanticId || Request.SemanticId == ComponentStateSemanticId)
    OutValue.ValueDefinition = BuildSpatialStateDefinition(Request.SemanticId == ComponentStateSemanticId);
  OutValue.bEditable = Request.SemanticId != WorldTargetSemanticId &&
                       Request.SemanticId != ActorTargetSemanticId &&
                       Request.SemanticId != ComponentTargetSemanticId;
  OutError.Reset();
  if (Request.Context.FindRef(TEXT("expected_absent")) == TEXT("true")) {
    OutValue.bAvailable = false;
    OutValue.AvailabilityReason = TEXT("target_not_found");
    return UeAgentAuthoringState::EReadOutcome::Unavailable;
  }

  if (Request.SemanticId == WorldStateSemanticId ||
      Request.SemanticId == WorldTargetSemanticId) {
    const UWorld *World = Cast<UWorld>(Request.RootObject);
    if (!World) {
      OutValue.bAvailable = false;
      OutValue.AvailabilityReason = TEXT("world_not_found");
      return UeAgentAuthoringState::EReadOutcome::Unavailable;
    }
    OutValue.Value = MakeShared<FJsonValueObject>(BuildWorldTarget(*World));
    return UeAgentAuthoringState::EReadOutcome::Resolved;
  }

  if (Request.SemanticId == ActorStateSemanticId ||
      Request.SemanticId == ActorTargetSemanticId) {
    const AActor *Actor = Cast<AActor>(Request.RootObject);
    if (!Actor) {
      OutValue.bAvailable = false;
      OutValue.AvailabilityReason = TEXT("actor_not_found");
      return UeAgentAuthoringState::EReadOutcome::Unavailable;
    }
    OutValue.Value = MakeShared<FJsonValueObject>(
        Request.SemanticId == ActorTargetSemanticId
            ? BuildActorTarget(*Actor, Request.TargetIdentity)
            : BuildActorState(*Actor, &Request.Context));
    return UeAgentAuthoringState::EReadOutcome::Resolved;
  }

  if (Request.SemanticId == ComponentStateSemanticId ||
      Request.SemanticId == ComponentTargetSemanticId) {
    const UActorComponent *Component =
        Cast<UActorComponent>(Request.RootObject);
    if (!Component) {
      OutValue.bAvailable = false;
      OutValue.AvailabilityReason = TEXT("component_not_found");
      return UeAgentAuthoringState::EReadOutcome::Unavailable;
    }
    OutValue.Value = MakeShared<FJsonValueObject>(
        Request.SemanticId == ComponentTargetSemanticId
            ? BuildComponentTarget(*Component, Request.TargetIdentity)
            : BuildComponentState(*Component, &Request.Context));
    return UeAgentAuthoringState::EReadOutcome::Resolved;
  }

  OutError = StateFailure(TEXT("level_state_semantic_unsupported"),
      TEXT("The Level state accessor does not support the requested semantic."), TEXT("semantic_supported"),
      MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
  OutError.SemanticId = Request.SemanticId;
  OutError.SourceKind = TEXT("native_result");
  return UeAgentAuthoringState::EReadOutcome::Failed;
}

UeAgentAuthoringNativeDefaults::EProbeOutcome
ProbeNativeDefault(const UeAgentAuthoringNativeDefaults::FRequest &Request,
                   UeAgentAuthoringNativeDefaults::FScratchScope &,
                   UeAgentAuthoringNativeDefaults::FProbeValue &OutValue,
                   UE::UAI::Contract::FDiagnostic& OutError) {
  OutValue = UeAgentAuthoringNativeDefaults::FProbeValue();
  OutError.Reset();
  const bool bActor = Request.TargetIdentity.TargetKind == TEXT("level_actor");
  const bool bComponent =
      Request.TargetIdentity.TargetKind == TEXT("level_component");
  if (!bActor && !bComponent) {
    return UeAgentAuthoringNativeDefaults::EProbeOutcome::Unavailable;
  }
  const FString ClassPath = Request.TargetIdentity.GetSelector(
      bActor ? TEXT("actor_class") : TEXT("component_class"));
  UClass *BaseClass =
      bActor ? AActor::StaticClass() : UActorComponent::StaticClass();
  UClass *Class = StaticLoadClass(BaseClass, nullptr, *ClassPath);
  UObject *DefaultObject = Class ? Class->GetDefaultObject() : nullptr;
  if (!DefaultObject) {
    OutError = StateFailure(TEXT("level_native_default_class_invalid"),
        Class ? TEXT("The requested class has no default object.") : TEXT("The requested class could not be loaded as the required Level object type."),
        bActor ? TEXT("actor_class") : TEXT("component_class"), MakeShared<FJsonValueString>(ClassPath),
        MakeShared<FJsonValueString>(BaseClass->GetPathName()));
    OutError.SourceKind = TEXT("native_result");
    return UeAgentAuthoringNativeDefaults::EProbeOutcome::Failed;
  }
  OutValue.Value = MakeShared<FJsonValueObject>(
      bActor
          ? BuildActorState(*CastChecked<AActor>(DefaultObject))
          : BuildComponentState(*CastChecked<UActorComponent>(DefaultObject)));
  OutValue.Provenance = NativeDefaultProviderId;
  return UeAgentAuthoringNativeDefaults::EProbeOutcome::Resolved;
}
} // namespace

FString GetWorldPath(const UWorld &World) {
  return World.GetOutermost()->GetName();
}

FString MakeManagedActorTag(const FString &ManagedId) {
  return FString::Printf(TEXT("UAI.Id=%s"), *ManagedId);
}

FName GetManagedActorMarkerTag() { return FName(TEXT("UAI.Managed")); }

bool IsManagedActorIdentityTag(const FName &Tag) {
  if (Tag == GetManagedActorMarkerTag())
    return true;
  const FString Text = Tag.ToString();
  return Text.StartsWith(TEXT("UAI.Id=")) || Text.StartsWith(TEXT("UAI.Id:"));
}

FString MakeManagedActorFolderPath(const FString &OwnerId) {
  const FString CleanOwnerId = OwnerId.TrimStartAndEnd();
  if (CleanOwnerId.IsEmpty())
    return FString();
  for (const TCHAR Character : CleanOwnerId) {
    if (!FChar::IsAlnum(Character) && Character != TEXT('_') &&
        Character != TEXT('-'))
      return FString();
  }
  return TEXT("UAI/Managed/") + CleanOwnerId;
}

bool TryBindManagedActorOwnership(AActor &Actor, const FString &ManagedId,
                                  const FString &OwnerId,
                                  UE::UAI::Contract::FDiagnostic& OutError) {
  OutError.Reset();
  const FString CleanManagedId = ManagedId.TrimStartAndEnd();
  const FString FolderPath = MakeManagedActorFolderPath(OwnerId);
  if (CleanManagedId.IsEmpty()) {
    OutError = StateFailure(TEXT("level_managed_actor_id_missing"), TEXT("A non-empty managed actor id is required."),
        TEXT("managed_id"), MakeShared<FJsonValueString>(ManagedId), MakeShared<FJsonValueString>(TEXT("non-empty string")));
    return false;
  }
  if (FolderPath.IsEmpty()) {
    OutError = StateFailure(TEXT("level_managed_actor_owner_invalid"),
        TEXT("Managed actor owner id must be non-empty and contain only letters, digits, '_' or '-'."),
        TEXT("owner_id"), MakeShared<FJsonValueString>(OwnerId),
        MakeShared<FJsonValueString>(TEXT("non-empty letters, digits, underscore or hyphen")));
    return false;
  }
  UWorld *const World = Actor.GetWorld();
  if (!World) {
    OutError = StateFailure(TEXT("level_managed_actor_world_unavailable"), TEXT("The managed actor has no world."),
        TEXT("world_present"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
    OutError.CandidateName = Actor.GetActorLabel();
    OutError.SourceKind = TEXT("native_result");
    return false;
  }
  for (const FString &ExistingManagedId : GetManagedActorIds(Actor)) {
    if (ExistingManagedId.IsEmpty() ||
        !ExistingManagedId.Equals(CleanManagedId,
                                  ESearchCase::CaseSensitive)) {
      OutError = StateFailure(TEXT("level_managed_actor_id_conflict"),
          TEXT("The actor already carries a different or empty managed id."), TEXT("managed_id_matches"),
          MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
      OutError.CandidateName = Actor.GetActorLabel();
      OutError.ActualEvidence = MakeShared<FJsonValueString>(ExistingManagedId);
      OutError.ExpectedEvidence = MakeShared<FJsonValueString>(CleanManagedId);
      OutError.SourceKind = TEXT("native_result");
      return false;
    }
  }
  for (TActorIterator<AActor> It(World); It; ++It) {
    AActor *const Candidate = *It;
    if (!Candidate || Candidate == &Actor || Candidate->IsActorBeingDestroyed())
      continue;
    for (const FString &ExistingManagedId : GetManagedActorIds(*Candidate)) {
      if (ExistingManagedId.Equals(CleanManagedId,
                                   ESearchCase::CaseSensitive)) {
        OutError = StateFailure(TEXT("level_managed_actor_id_duplicate"),
            TEXT("Another actor in the world already carries the requested managed id."), TEXT("managed_id_unique"),
            MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
        OutError.CandidateName = Candidate->GetActorLabel();
        OutError.TargetPath = GetWorldPath(*World);
        OutError.InternalContext = CleanManagedId;
        OutError.SourceKind = TEXT("native_result");
        return false;
      }
    }
  }

  const TArray<FName> OriginalTags = Actor.Tags;
  const FName OriginalFolder = Actor.GetFolderPath();
  Actor.Modify();
  Actor.Tags.RemoveAll(
      [](const FName &Tag) {
        return IsManagedActorIdentityTag(Tag)
            && Tag != GetManagedActorMarkerTag();
      });
  Actor.Tags.AddUnique(GetManagedActorMarkerTag());
  Actor.Tags.AddUnique(FName(*MakeManagedActorTag(CleanManagedId)));
  Actor.SetFolderPath(FName(*FolderPath));
  const TArray<FString> AppliedManagedIds = GetManagedActorIds(Actor);
  if (!Actor.ActorHasTag(GetManagedActorMarkerTag()) ||
      AppliedManagedIds.Num() != 1 ||
      !AppliedManagedIds[0].Equals(CleanManagedId,
                                   ESearchCase::CaseSensitive) ||
      !GetActorFolderPathText(Actor).Equals(FolderPath,
                                            ESearchCase::CaseSensitive)) {
    const bool bMarkerPresent = Actor.ActorHasTag(GetManagedActorMarkerTag());
    const FString ObservedFolder = GetActorFolderPathText(Actor);
    OutError = StateFailure(TEXT("level_managed_actor_ownership_apply_failed"),
        !bMarkerPresent ? TEXT("The managed actor marker was not retained after applying ownership.")
        : AppliedManagedIds.Num() != 1 ? TEXT("The actor did not retain exactly one managed id after applying ownership.")
        : !AppliedManagedIds[0].Equals(CleanManagedId, ESearchCase::CaseSensitive)
            ? TEXT("The actor did not retain the requested managed id after applying ownership.")
            : TEXT("The actor did not retain the requested managed folder after applying ownership."),
        !bMarkerPresent ? TEXT("managed_marker_present") : AppliedManagedIds.Num() != 1 ? TEXT("managed_id_count")
            : !AppliedManagedIds[0].Equals(CleanManagedId, ESearchCase::CaseSensitive) ? TEXT("managed_id_matches") : TEXT("folder_path"),
        !bMarkerPresent ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false))
            : AppliedManagedIds.Num() != 1 ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(AppliedManagedIds.Num()))
            : !AppliedManagedIds[0].Equals(CleanManagedId, ESearchCase::CaseSensitive) ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false))
            : MakeShared<FJsonValueString>(ObservedFolder),
        !bMarkerPresent ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(true))
            : AppliedManagedIds.Num() != 1 ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(1))
            : !AppliedManagedIds[0].Equals(CleanManagedId, ESearchCase::CaseSensitive) ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(true))
            : MakeShared<FJsonValueString>(FolderPath));
    OutError.CandidateName = Actor.GetActorLabel();
    OutError.SourceKind = TEXT("native_result");
    Actor.Tags = OriginalTags;
    Actor.SetFolderPath(OriginalFolder);
    return false;
  }
  return true;
}

bool TryFindManagedActorExact(UWorld &World, const FString &ManagedId,
                              const FString &ExpectedFolderPath,
                              AActor *&OutActor, UE::UAI::Contract::FDiagnostic& OutError) {
  OutActor = nullptr;
  OutError.Reset();
  const FString CleanManagedId = ManagedId.TrimStartAndEnd();
  const FString CleanFolderPath = ExpectedFolderPath.TrimStartAndEnd();
  if (CleanManagedId.IsEmpty()) {
    OutError = StateFailure(TEXT("level_managed_actor_id_missing"), TEXT("A non-empty managed actor id is required."),
        TEXT("managed_id"), MakeShared<FJsonValueString>(ManagedId), MakeShared<FJsonValueString>(TEXT("non-empty string")));
    return false;
  }
  if (CleanFolderPath.IsEmpty()) {
    OutError = StateFailure(TEXT("level_managed_actor_folder_missing"), TEXT("The expected managed actor folder is required."),
        TEXT("folder_path"), MakeShared<FJsonValueString>(ExpectedFolderPath), MakeShared<FJsonValueString>(TEXT("non-empty folder path")));
    return false;
  }

  TArray<AActor *> Matches;
  for (TActorIterator<AActor> It(&World); It; ++It) {
    AActor *const Candidate = *It;
    if (!Candidate || Candidate->IsActorBeingDestroyed())
      continue;
    const TArray<FString> CandidateIds = GetManagedActorIds(*Candidate);
    if (CandidateIds.ContainsByPredicate([&CleanManagedId](const FString &Id) {
          return Id.Equals(CleanManagedId, ESearchCase::CaseSensitive);
        }))
      Matches.Add(Candidate);
  }
  if (Matches.IsEmpty()) {
    OutError = StateFailure(TEXT("level_managed_actor_not_found"), TEXT("No actor in the world carries the requested managed id."),
        TEXT("matching_actor_count"), MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueNumber>(1));
    OutError.TargetPath = GetWorldPath(World);
    OutError.InternalContext = CleanManagedId;
    OutError.SourceKind = TEXT("native_result");
    return false;
  }
  if (Matches.Num() != 1) {
    OutError = StateFailure(TEXT("level_managed_actor_id_duplicate"), TEXT("More than one actor carries the requested managed id."),
        TEXT("matching_actor_count"), MakeShared<FJsonValueNumber>(Matches.Num()), MakeShared<FJsonValueNumber>(1));
    TArray<TSharedPtr<FJsonValue>> Labels;
    for (const AActor* Match : Matches) Labels.Add(MakeShared<FJsonValueString>(Match->GetActorLabel()));
    OutError.CandidateChoices = MakeShared<FJsonValueArray>(MoveTemp(Labels));
    OutError.TargetPath = GetWorldPath(World);
    OutError.InternalContext = CleanManagedId;
    OutError.SourceKind = TEXT("native_result");
    return false;
  }

  AActor *const Candidate = Matches[0];
  const TArray<FString> CandidateIds = GetManagedActorIds(*Candidate);
  if (CandidateIds.Num() != 1) {
    OutError = StateFailure(TEXT("level_managed_actor_identity_ambiguous"), TEXT("The matching actor carries multiple managed ids."),
        TEXT("managed_id_count"), MakeShared<FJsonValueNumber>(CandidateIds.Num()), MakeShared<FJsonValueNumber>(1));
    OutError.CandidateName = Candidate->GetActorLabel();
    OutError.InternalContext = Candidate->GetPathName();
    OutError.SourceKind = TEXT("native_result");
    return false;
  }
  if (!Candidate->ActorHasTag(GetManagedActorMarkerTag())) {
    OutError = StateFailure(TEXT("level_managed_actor_marker_missing"), TEXT("The matching actor is missing the managed-actor marker."),
        TEXT("managed_marker_present"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
    OutError.CandidateName = Candidate->GetActorLabel();
    OutError.InternalContext = Candidate->GetPathName();
    OutError.SourceKind = TEXT("native_result");
    return false;
  }
  const FString ObservedFolder = GetActorFolderPathText(*Candidate);
  if (!ObservedFolder.Equals(CleanFolderPath, ESearchCase::CaseSensitive)) {
    OutError = StateFailure(TEXT("level_managed_actor_folder_mismatch"), TEXT("The matching actor is in a different folder."),
        TEXT("folder_path"), MakeShared<FJsonValueString>(ObservedFolder), MakeShared<FJsonValueString>(CleanFolderPath));
    OutError.CandidateName = Candidate->GetActorLabel();
    OutError.SourceKind = TEXT("native_result");
    return false;
  }
  OutActor = Candidate;
  return true;
}

FString GetManagedActorId(const AActor &Actor) {
  for (const FName &Tag : Actor.Tags) {
    const FString Text = Tag.ToString();
    if (Text.StartsWith(TEXT("UAI.Id=")) || Text.StartsWith(TEXT("UAI.Id:"))) {
      return Text.Mid(7);
    }
  }
  return FString();
}

FString GetActorFolderPathText(const AActor &Actor) {
  const FName FolderPath = Actor.GetFolderPath();
  return FolderPath.IsNone() ? FString() : FolderPath.ToString();
}

namespace {
void CollectDataLayerIdentityCandidates(
    AWorldDataLayers &WorldDataLayers, const EDataLayerTargetScope Scope,
    TArray<UDataLayerInstance *> &OutCandidates,
    TArray<TArray<FString>> &OutCandidateIdentities) {
  OutCandidates.Reset();
  OutCandidateIdentities.Reset();
  WorldDataLayers.ForEachDataLayerInstance(
      [&OutCandidates, &OutCandidateIdentities,
       Scope](UDataLayerInstance *Instance) {
        if (!Instance)
          return true;
        const UDataLayerAsset *const Asset = Instance->GetAsset();
        if (Scope == EDataLayerTargetScope::PrivateOnly &&
            (!Asset || !Asset->IsPrivate()))
          return true;
        OutCandidates.Add(Instance);
        OutCandidateIdentities.Add({
            Instance->GetDataLayerFName().ToString(),
            Instance->GetDataLayerShortName(),
            Instance->GetDataLayerFullName(),
            Instance->GetPathName(),
            Asset ? Asset->GetPathName() : FString(),
        });
        return true;
      });
}

TArray<int32> FindExactDataLayerIdentityCandidateIndices(
    const TArray<TArray<FString>> &CandidateIdentities,
    const FString &TargetRef) {
  TArray<int32> Matches;
  const FString CleanTarget = TargetRef.TrimStartAndEnd();
  if (CleanTarget.IsEmpty())
    return Matches;
  for (int32 CandidateIndex = 0;
       CandidateIndex < CandidateIdentities.Num(); ++CandidateIndex) {
    if (CandidateIdentities[CandidateIndex].ContainsByPredicate(
            [&CleanTarget](const FString &Identity) {
              return !Identity.IsEmpty() && Identity.Equals(
                  CleanTarget, ESearchCase::CaseSensitive);
            }))
      Matches.Add(CandidateIndex);
  }
  return Matches;
}
} // namespace

TArray<UDataLayerInstance *> FindDataLayerInstancesExact(
    AWorldDataLayers &WorldDataLayers, const FString &TargetRef,
    const EDataLayerTargetScope Scope) {
  TArray<UDataLayerInstance *> Matches;
  TArray<UDataLayerInstance *> Candidates;
  TArray<TArray<FString>> CandidateIdentities;
  CollectDataLayerIdentityCandidates(
      WorldDataLayers, Scope, Candidates, CandidateIdentities);
  for (const int32 CandidateIndex :
       FindExactDataLayerIdentityCandidateIndices(
           CandidateIdentities, TargetRef))
    Matches.Add(Candidates[CandidateIndex]);
  return Matches;
}

bool TrySelectDataLayerIdentityExact(
    const TArray<TArray<FString>> &CandidateIdentities,
    const FString &TargetRef, int32 &OutCandidateIndex, UE::UAI::Contract::FDiagnostic& OutError) {
  OutCandidateIndex = INDEX_NONE;
  OutError.Reset();
  const FString CleanTarget = TargetRef.TrimStartAndEnd();
  if (CleanTarget.IsEmpty()) {
    OutError = StateFailure(TEXT("world_data_layer_target_required"), TEXT("A non-empty data-layer target is required."),
        TEXT("data_layer"), MakeShared<FJsonValueString>(TargetRef), MakeShared<FJsonValueString>(TEXT("non-empty data-layer identity")));
    return false;
  }

  const TArray<int32> Matches = FindExactDataLayerIdentityCandidateIndices(
      CandidateIdentities, CleanTarget);
  if (Matches.IsEmpty()) {
    OutError = StateFailure(TEXT("world_data_layer_missing"), TEXT("No data layer matches the requested identity."),
        TEXT("matching_data_layer_count"), MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueNumber>(1));
    OutError.CandidateName = CleanTarget;
    return false;
  }
  if (Matches.Num() != 1) {
    OutError = StateFailure(TEXT("world_data_layer_identity_ambiguous"), TEXT("More than one data layer matches the requested identity."),
        TEXT("matching_data_layer_count"), MakeShared<FJsonValueNumber>(Matches.Num()), MakeShared<FJsonValueNumber>(1));
    OutError.CandidateName = CleanTarget;
    TArray<TSharedPtr<FJsonValue>> Choices;
    for (int32 Match : Matches) {
      TArray<TSharedPtr<FJsonValue>> Names;
      for (const FString& Name : CandidateIdentities[Match]) Names.Add(MakeShared<FJsonValueString>(Name));
      Choices.Add(MakeShared<FJsonValueArray>(MoveTemp(Names)));
    }
    OutError.CandidateChoices = MakeShared<FJsonValueArray>(MoveTemp(Choices));
    return false;
  }
  OutCandidateIndex = Matches[0];
  return true;
}

bool TryResolveDataLayerInstanceExact(
    AWorldDataLayers &WorldDataLayers, const FString &TargetRef,
    UDataLayerInstance *&OutInstance, UE::UAI::Contract::FDiagnostic& OutError,
    const EDataLayerTargetScope Scope) {
  OutInstance = nullptr;
  OutError.Reset();
  const FString CleanTarget = TargetRef.TrimStartAndEnd();
  if (CleanTarget.IsEmpty()) {
    OutError = StateFailure(TEXT("world_data_layer_target_required"), TEXT("A non-empty data-layer target is required."),
        TEXT("data_layer"), MakeShared<FJsonValueString>(TargetRef), MakeShared<FJsonValueString>(TEXT("non-empty data-layer identity")));
    return false;
  }

  TArray<UDataLayerInstance *> Candidates;
  TArray<TArray<FString>> CandidateIdentities;
  CollectDataLayerIdentityCandidates(
      WorldDataLayers, Scope, Candidates, CandidateIdentities);

  int32 CandidateIndex = INDEX_NONE;
  if (!TrySelectDataLayerIdentityExact(
          CandidateIdentities, CleanTarget, CandidateIndex, OutError))
    return false;
  OutInstance = Candidates[CandidateIndex];
  return true;
}

UWorld *FindEditorWorldByPath(const FString &WorldPath) {
  if (GEditor) {
    if (UWorld *World = GEditor->GetEditorWorldContext().World()) {
      if (GetWorldPath(*World).Equals(WorldPath, ESearchCase::IgnoreCase))
        return World;
    }
  }
  if (GEngine) {
    for (const FWorldContext &Context : GEngine->GetWorldContexts()) {
      UWorld *World = Context.World();
      if (World &&
          GetWorldPath(*World).Equals(WorldPath, ESearchCase::IgnoreCase))
        return World;
    }
  }
  return nullptr;
}

AActor *FindActorExact(UWorld &World,
                       const TSharedPtr<const FJsonObject> &Target,
                       UE::UAI::Contract::FDiagnostic& OutError) {
  OutError.Reset();
  if (!Target.IsValid()) {
    OutError = StateFailure(TEXT("level_actor_target_missing"), TEXT("An actor target object is required."),
        TEXT("target"), nullptr, MakeShared<FJsonValueString>(TEXT("actor target object")));
    return nullptr;
  }
  FString WorldPath;
  Target->TryGetStringField(TEXT("world_path"), WorldPath);
  if (!GetWorldPath(World).Equals(WorldPath, ESearchCase::IgnoreCase)) {
    OutError = StateFailure(TEXT("level_world_path_conflict"), TEXT("The requested world_path does not identify the world being searched."),
        TEXT("world_path"), Target->TryGetField(TEXT("world_path")), MakeShared<FJsonValueString>(GetWorldPath(World)), Target.Get());
    return nullptr;
  }

  FString GuidString;
  FString ManagedId;
  FString ObjectPath;
  Target->TryGetStringField(TEXT("actor_guid"), GuidString);
  Target->TryGetStringField(TEXT("managed_id"), ManagedId);
  Target->TryGetStringField(TEXT("object_path"), ObjectPath);
  FGuid Guid;
  if (!GuidString.IsEmpty() && !ParseGuid(GuidString, Guid)) {
    OutError = StateFailure(TEXT("level_actor_guid_invalid"), TEXT("actor_guid must be a parseable, non-zero GUID."),
        TEXT("actor_guid"), Target->TryGetField(TEXT("actor_guid")), MakeShared<FJsonValueString>(TEXT("non-zero GUID")), Target.Get());
    return nullptr;
  }
  if (GuidString.IsEmpty() && ManagedId.IsEmpty() && ObjectPath.IsEmpty()) {
    OutError = StateFailure(TEXT("level_actor_identity_missing"), TEXT("Actor lookup requires actor_guid, managed_id or object_path."),
        TEXT("selector_count"), MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("at least one actor selector")), Target.Get());
    return nullptr;
  }

  AActor *Match = nullptr;
  for (TActorIterator<AActor> It(&World); It; ++It) {
    AActor *Candidate = *It;
    if (!Candidate)
      continue;
    const bool bGuidMatch =
        GuidString.IsEmpty() || Candidate->GetActorGuid() == Guid;
    const bool bManagedMatch =
        ManagedId.IsEmpty() ||
        GetManagedActorId(*Candidate)
            .Equals(ManagedId, ESearchCase::CaseSensitive);
    const bool bObjectMatch =
        ObjectPath.IsEmpty() ||
        Candidate->GetPathName().Equals(ObjectPath, ESearchCase::CaseSensitive);
    if (!bGuidMatch || !bManagedMatch || !bObjectMatch)
      continue;
    if (Match) {
      OutError = StateFailure(TEXT("level_actor_identity_ambiguous"), TEXT("The actor selectors match more than one actor."),
          TEXT("actor_identity_unique"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Target.Get());
      OutError.TargetPath = GetWorldPath(World);
      OutError.CandidateChoices = MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
          MakeShared<FJsonValueString>(Match->GetActorLabel()), MakeShared<FJsonValueString>(Candidate->GetActorLabel())});
      return nullptr;
    }
    Match = Candidate;
  }
  if (Match) {
    const FAuthorabilityResult Authorability =
        EvaluateActorAuthorability(World, *Match);
    if (!Authorability.bAuthorable) {
      OutError = Authorability.Diagnostic;
      return nullptr;
    }
  }
  return Match;
}

UActorComponent *FindComponentExact(AActor &Owner, const FString &ComponentName,
                                    UE::UAI::Contract::FDiagnostic& OutError) {
  OutError.Reset();
  if (ComponentName.IsEmpty()) {
    OutError = StateFailure(TEXT("level_component_name_missing"), TEXT("Component lookup requires a non-empty component name."),
        TEXT("component_name"), MakeShared<FJsonValueString>(ComponentName), MakeShared<FJsonValueString>(TEXT("non-empty string")));
    return nullptr;
  }
  UWorld *World = Owner.GetWorld();
  if (!World) {
    OutError = StateFailure(TEXT("level_component_not_authorable"), TEXT("The component owner has no world."),
        TEXT("owner_world_present"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
    OutError.CandidateName = ComponentName;
    OutError.SourceKind = TEXT("native_result");
    return nullptr;
  }
  const FAuthorabilityResult OwnerAuthorability =
      EvaluateActorAuthorability(*World, Owner);
  if (!OwnerAuthorability.bAuthorable) {
    OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("level_component_not_authorable"),
        OwnerAuthorability.Diagnostic, TEXT("state_capture"), TEXT("The component owner cannot be authored."));
    if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = ComponentName;
    return nullptr;
  }
  UActorComponent *Match = nullptr;
  TArray<UActorComponent *> Components;
  Owner.GetComponents(Components);
  for (UActorComponent *Candidate : Components) {
    if (!Candidate ||
        !Candidate->GetName().Equals(ComponentName, ESearchCase::CaseSensitive))
      continue;
    if (Match) {
      OutError = StateFailure(TEXT("level_component_identity_ambiguous"), TEXT("The owner contains more than one component with the requested name."),
          TEXT("component_name_unique"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
      OutError.CandidateName = ComponentName;
      OutError.TargetPath = GetWorldPath(*World);
      OutError.SourceKind = TEXT("native_result");
      return nullptr;
    }
    Match = Candidate;
  }
  if (Match) {
    const FAuthorabilityResult Authorability =
        EvaluateComponentAuthorability(*World, *Match);
    if (!Authorability.bAuthorable) {
      OutError = Authorability.Diagnostic;
      return nullptr;
    }
  }
  return Match;
}

bool BuildWorldIdentity(
    const UWorld &World,
    UeAgentAuthoringIdentity::FCanonicalIdentity &OutIdentity,
    UE::UAI::Contract::FDiagnostic& OutError) {
  return BuildIdentity(WorldIdentityModelId, TEXT("level_world"),
                       {{TEXT("world_path"), GetWorldPath(World)}}, OutIdentity,
                       OutError);
}

bool BuildActorIdentity(
    const AActor &Actor,
    UeAgentAuthoringIdentity::FCanonicalIdentity &OutIdentity,
    UE::UAI::Contract::FDiagnostic& OutError) {
  return BuildIdentity(ActorIdentityModelId, TEXT("level_actor"),
                       ActorSelectors(Actor), OutIdentity, OutError);
}

bool BuildComponentIdentity(
    const UActorComponent &Component,
    UeAgentAuthoringIdentity::FCanonicalIdentity &OutIdentity,
    UE::UAI::Contract::FDiagnostic& OutError) {
  return BuildIdentity(ComponentIdentityModelId, TEXT("level_component"),
                       ComponentSelectors(Component), OutIdentity, OutError);
}

bool BuildWorldSnapshotReadRequests(
    UWorld *World, const FString &WorldPath, const bool bExpectedAbsent,
    TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
    UE::UAI::Contract::FDiagnostic& OutError) {
  OutRequests.Reset();
  UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
  if (!BuildIdentity(WorldIdentityModelId, TEXT("level_world"),
                     {{TEXT("world_path"), WorldPath}}, Identity, OutError)) {
    return false;
  }
  return AddReadRequest(WorldStateSemanticId, Identity, World, bExpectedAbsent,
                        OutRequests);
}

bool BuildActorSnapshotReadRequests(
    AActor *Actor, const UeAgentAuthoringIdentity::FCanonicalIdentity &Identity,
    const bool bExpectedAbsent,
    TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
    UE::UAI::Contract::FDiagnostic& OutError) {
  OutRequests.Reset();
  OutError.Reset();
  if (!Identity.IsValid()) {
    OutError = StateFailure(TEXT("level_actor_snapshot_identity_invalid"),
        TEXT("The actor snapshot requires a valid canonical identity."), TEXT("actor_identity_valid"),
        MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
    OutError.InternalContext = Identity.CanonicalKey;
    OutError.SourceKind = TEXT("native_result");
    return false;
  }
  return AddReadRequest(ActorStateSemanticId, Identity, Actor, bExpectedAbsent, OutRequests);
}

bool BuildComponentSnapshotReadRequests(
    UActorComponent *Component,
    const UeAgentAuthoringIdentity::FCanonicalIdentity &Identity,
    const bool bExpectedAbsent,
    TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
    UE::UAI::Contract::FDiagnostic& OutError) {
  OutRequests.Reset();
  OutError.Reset();
  if (!Identity.IsValid()) {
    OutError = StateFailure(TEXT("level_component_snapshot_identity_invalid"),
        TEXT("The component snapshot requires a valid canonical identity."), TEXT("component_identity_valid"),
        MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
    OutError.InternalContext = Identity.CanonicalKey;
    OutError.SourceKind = TEXT("native_result");
    return false;
  }
  return AddReadRequest(ComponentStateSemanticId, Identity, Component, bExpectedAbsent, OutRequests);
}

bool BuildWorldExportReadRequests(
    UWorld &World, TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
    UE::UAI::Contract::FDiagnostic& OutError) {
  OutRequests.Reset();
  UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
  if (!BuildWorldIdentity(World, Identity, OutError) ||
      !AddReadRequest(WorldTargetSemanticId, Identity, &World, false,
                      OutRequests)) return false;
  const AWorldSettings* Settings = World.GetWorldSettings();
  if (!Settings) {
    OutError = StateFailure(TEXT("level_world_settings_missing"), TEXT("The world has no WorldSettings object to export."),
        TEXT("world_settings_present"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
    OutError.TargetPath = GetWorldPath(World);
    OutError.SourceKind = TEXT("native_result");
    return false;
  }
  return AppendEditableProperties(
      Settings, WorldPropertySemanticId, WorldPropertyIdentityModelId,
      TEXT("level_world_property"), {{TEXT("world_path"), GetWorldPath(World)}},
      {TEXT("ActorGuid"), TEXT("RootComponent"), TEXT("Tags")}, OutRequests,
      OutError);
}

bool BuildActorExportReadRequests(
    AActor &Actor, TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
    UE::UAI::Contract::FDiagnostic& OutError) {
  OutRequests.Reset();
  UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
  const TMap<FString, FString> Selectors = ActorSelectors(Actor);
  if (!BuildIdentity(ActorIdentityModelId, TEXT("level_actor"), Selectors,
                     Identity, OutError) ||
      !AddReadRequest(ActorTargetSemanticId, Identity, &Actor, false,
                      OutRequests) ||
      !AddReadRequest(ActorStateSemanticId, Identity, &Actor, false,
                      OutRequests)) {
    return false;
  }
  return AppendEditableProperties(
      &Actor, ActorPropertySemanticId, ActorPropertyIdentityModelId,
      TEXT("level_actor_property"), Selectors,
      {TEXT("ActorGuid"), TEXT("RootComponent"), TEXT("Tags")}, OutRequests,
      OutError);
}

bool BuildComponentExportReadRequests(
    UActorComponent &Component,
    TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
    UE::UAI::Contract::FDiagnostic& OutError) {
  OutRequests.Reset();
  UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
  const TMap<FString, FString> Selectors = ComponentSelectors(Component);
  if (!BuildIdentity(ComponentIdentityModelId, TEXT("level_component"),
                     Selectors, Identity, OutError) ||
      !AddReadRequest(ComponentTargetSemanticId, Identity, &Component, false,
                      OutRequests) ||
      !AddReadRequest(ComponentStateSemanticId, Identity, &Component, false,
                      OutRequests)) {
    return false;
  }
  TSet<FName> Excluded =
      {TEXT("AttachParent"), TEXT("AttachSocketName"), TEXT("RelativeLocation"),
       TEXT("RelativeRotation"), TEXT("RelativeScale3D"),
       TEXT("CreationMethod")};
  if (Component.IsA<UPCGComponent>()) Excluded.Add(TEXT("GraphInstance"));
  return AppendEditableProperties(
      &Component, ComponentPropertySemanticId, ComponentPropertyIdentityModelId,
      TEXT("level_component_property"), Selectors, Excluded,
      OutRequests, OutError);
}

bool RegisterStateAccessor(UE::UAI::Contract::FDiagnostic& OutError) {
  UeAgentAuthoringState::FStateAccessorDescriptor Descriptor;
  Descriptor.AccessorId = StateAccessorId;
  Descriptor.TargetKinds = {TEXT("level_world"), TEXT("level_actor"),
                            TEXT("level_component")};
  Descriptor.CapabilityTags = {TEXT("world"), TEXT("actor"), TEXT("component"),
                               TEXT("attachment"), TEXT("transform")};
  UeAgentAuthoringState::ERegistrationResult Result;
  return UeAgentAuthoringState::FStateAccessorRegistry::Get().RegisterAccessor(
      Descriptor, ReadState, Result, OutError);
}

bool RegisterNativeDefaultProvider(UE::UAI::Contract::FDiagnostic& OutError) {
  UeAgentAuthoringNativeDefaults::FProviderDescriptor Descriptor;
  Descriptor.ProviderId = NativeDefaultProviderId;
  Descriptor.SourceKind =
      UeAgentAuthoringSemanticRegistry::EDefaultSourceKind::Factory;
  Descriptor.TargetKinds = {TEXT("level_actor"), TEXT("level_component")};
  Descriptor.CapabilityTags = {TEXT("class_default_object"), TEXT("actor"),
                               TEXT("component"), TEXT("native_default")};
  Descriptor.bRequiresScratch = false;
  UeAgentAuthoringNativeDefaults::ERegistrationResult Result;
  return UeAgentAuthoringNativeDefaults::FProviderRegistry::Get()
      .RegisterProvider(Descriptor, ProbeNativeDefault, Result, OutError);
}
} // namespace UeAgentLevelAuthoring
