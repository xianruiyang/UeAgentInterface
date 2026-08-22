// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Level/Authoring/UeAgentLevelAuthoringState.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringAuthorability.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringDescriptors.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringSpatial.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "UObject/UObjectGlobals.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringNativeDefaults.h"

namespace UeAgentLevelAuthoring {
namespace {
bool IsManagedActorIdTag(const FName &Tag) {
  const FString Text = Tag.ToString();
  return Text.StartsWith(TEXT("UAI.Id=")) || Text.StartsWith(TEXT("UAI.Id:"));
}

TArray<FString> GetManagedActorIds(const AActor &Actor) {
  TArray<FString> Result;
  for (const FName &Tag : Actor.Tags) {
    if (IsManagedActorIdTag(Tag))
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
                   FString &OutError) {
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
    FString &OutError) {
  UeAgentAuthoringState::FEditablePropertyReadOptions Options;
  Options.SemanticId = SemanticId;
  Options.ExcludedProperties = Excluded;
  Options.BuildSnapshotIdentity =
      [ModelId, TargetKind,
       BaseSelectors](const FString &PropertyPath,
                      UeAgentAuthoringIdentity::FCanonicalIdentity &OutIdentity,
                      FString &IdentityError) {
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
  const FString KeyPrefix(Prefix);
  if (Context->FindRef(KeyPrefix + TEXT("transform_override")) != TEXT("true"))
    return false;
  FVector Location;
  FRotator Rotation;
  FVector Scale;
  if (!Location.InitFromString(
          Context->FindRef(KeyPrefix + TEXT("transform_location"))) ||
      !Rotation.InitFromString(
          Context->FindRef(KeyPrefix + TEXT("transform_rotation"))) ||
      !Scale.InitFromString(
          Context->FindRef(KeyPrefix + TEXT("transform_scale")))) {
    return false;
  }
  OutTransform = FTransform(Rotation, Location, Scale);
  return true;
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
            : Actor.GetActorLabel());
  }
  Result->SetStringField(
      TEXT("folder_path"),
      Context && Context->Contains(TEXT("actor_folder_override"))
          ? Context->FindRef(TEXT("actor_folder_override"))
          : GetActorFolderPathText(Actor));
  FTransform Transform = Actor.GetActorTransform();
  ReadTransformOverride(Context, TEXT("actor_"), Transform);
  Result->SetObjectField(
      TEXT("transform"),
      BuildTransformState(Transform, ETransformFrame::ActorWorld));
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

UeAgentAuthoringState::EReadOutcome
ReadState(const UeAgentAuthoringState::FReadRequest &Request,
          UeAgentAuthoringState::FStateValue &OutValue, FString &OutError) {
  OutValue = UeAgentAuthoringState::FStateValue();
  OutValue.TargetIdentity = Request.TargetIdentity;
  OutValue.Provenance = StateAccessorId;
  OutValue.Type.Kind = TEXT("object");
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

  OutError = TEXT("level_state_semantic_unsupported:") + Request.SemanticId;
  return UeAgentAuthoringState::EReadOutcome::Failed;
}

UeAgentAuthoringNativeDefaults::EProbeOutcome
ProbeNativeDefault(const UeAgentAuthoringNativeDefaults::FRequest &Request,
                   UeAgentAuthoringNativeDefaults::FScratchScope &,
                   UeAgentAuthoringNativeDefaults::FProbeValue &OutValue,
                   FString &OutError) {
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
    OutError = TEXT("level_native_default_class_invalid:") + ClassPath;
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
                                  FString &OutError) {
  OutError.Reset();
  const FString CleanManagedId = ManagedId.TrimStartAndEnd();
  const FString FolderPath = MakeManagedActorFolderPath(OwnerId);
  if (CleanManagedId.IsEmpty()) {
    OutError = TEXT("level_managed_actor_id_missing");
    return false;
  }
  if (FolderPath.IsEmpty()) {
    OutError = TEXT("level_managed_actor_owner_invalid:") + OwnerId;
    return false;
  }
  UWorld *const World = Actor.GetWorld();
  if (!World) {
    OutError = TEXT("level_managed_actor_world_unavailable");
    return false;
  }
  for (const FString &ExistingManagedId : GetManagedActorIds(Actor)) {
    if (ExistingManagedId.IsEmpty() ||
        !ExistingManagedId.Equals(CleanManagedId,
                                  ESearchCase::CaseSensitive)) {
      OutError = TEXT("level_managed_actor_id_conflict:") + ExistingManagedId;
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
        OutError = TEXT("level_managed_actor_id_duplicate:") + CleanManagedId;
        return false;
      }
    }
  }

  const TArray<FName> OriginalTags = Actor.Tags;
  const FName OriginalFolder = Actor.GetFolderPath();
  Actor.Modify();
  Actor.Tags.RemoveAll(
      [](const FName &Tag) { return IsManagedActorIdTag(Tag); });
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
    Actor.Tags = OriginalTags;
    Actor.SetFolderPath(OriginalFolder);
    OutError = TEXT("level_managed_actor_ownership_apply_failed");
    return false;
  }
  return true;
}

bool TryFindManagedActorExact(UWorld &World, const FString &ManagedId,
                              const FString &ExpectedFolderPath,
                              AActor *&OutActor, FString &OutError) {
  OutActor = nullptr;
  OutError.Reset();
  const FString CleanManagedId = ManagedId.TrimStartAndEnd();
  const FString CleanFolderPath = ExpectedFolderPath.TrimStartAndEnd();
  if (CleanManagedId.IsEmpty()) {
    OutError = TEXT("level_managed_actor_id_missing");
    return false;
  }
  if (CleanFolderPath.IsEmpty()) {
    OutError = TEXT("level_managed_actor_folder_missing");
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
    OutError = TEXT("level_managed_actor_not_found:") + CleanManagedId;
    return false;
  }
  if (Matches.Num() != 1) {
    OutError = TEXT("level_managed_actor_id_duplicate:") + CleanManagedId;
    return false;
  }

  AActor *const Candidate = Matches[0];
  const TArray<FString> CandidateIds = GetManagedActorIds(*Candidate);
  if (CandidateIds.Num() != 1) {
    OutError = TEXT("level_managed_actor_identity_ambiguous:")
        + Candidate->GetPathName();
    return false;
  }
  if (!Candidate->ActorHasTag(GetManagedActorMarkerTag())) {
    OutError = TEXT("level_managed_actor_marker_missing:")
        + Candidate->GetPathName();
    return false;
  }
  const FString ObservedFolder = GetActorFolderPathText(*Candidate);
  if (!ObservedFolder.Equals(CleanFolderPath, ESearchCase::CaseSensitive)) {
    OutError = TEXT("level_managed_actor_folder_mismatch:") + ObservedFolder;
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
                       FString &OutError) {
  OutError.Reset();
  if (!Target.IsValid()) {
    OutError = TEXT("level_actor_target_missing");
    return nullptr;
  }
  FString WorldPath;
  Target->TryGetStringField(TEXT("world_path"), WorldPath);
  if (!GetWorldPath(World).Equals(WorldPath, ESearchCase::IgnoreCase)) {
    OutError = TEXT("level_world_path_conflict:") + WorldPath;
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
    OutError = TEXT("level_actor_guid_invalid:") + GuidString;
    return nullptr;
  }
  if (GuidString.IsEmpty() && ManagedId.IsEmpty() && ObjectPath.IsEmpty()) {
    OutError = TEXT("level_actor_identity_missing");
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
      OutError = TEXT("level_actor_identity_ambiguous");
      return nullptr;
    }
    Match = Candidate;
  }
  if (Match) {
    const FAuthorabilityResult Authorability =
        EvaluateActorAuthorability(World, *Match);
    if (!Authorability.bAuthorable) {
      OutError = Authorability.ErrorDetail;
      return nullptr;
    }
  }
  return Match;
}

UActorComponent *FindComponentExact(AActor &Owner, const FString &ComponentName,
                                    FString &OutError) {
  OutError.Reset();
  if (ComponentName.IsEmpty()) {
    OutError = TEXT("level_component_name_missing");
    return nullptr;
  }
  UWorld *World = Owner.GetWorld();
  if (!World) {
    OutError = TEXT("level_component_not_authorable:owner_not_authorable");
    return nullptr;
  }
  const FAuthorabilityResult OwnerAuthorability =
      EvaluateActorAuthorability(*World, Owner);
  if (!OwnerAuthorability.bAuthorable) {
    OutError = TEXT("level_component_not_authorable:owner_not_authorable");
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
      OutError = TEXT("level_component_identity_ambiguous:") + ComponentName;
      return nullptr;
    }
    Match = Candidate;
  }
  if (Match) {
    const FAuthorabilityResult Authorability =
        EvaluateComponentAuthorability(*World, *Match);
    if (!Authorability.bAuthorable) {
      OutError = Authorability.ErrorDetail;
      return nullptr;
    }
  }
  return Match;
}

bool BuildWorldIdentity(
    const UWorld &World,
    UeAgentAuthoringIdentity::FCanonicalIdentity &OutIdentity,
    FString &OutError) {
  return BuildIdentity(WorldIdentityModelId, TEXT("level_world"),
                       {{TEXT("world_path"), GetWorldPath(World)}}, OutIdentity,
                       OutError);
}

bool BuildActorIdentity(
    const AActor &Actor,
    UeAgentAuthoringIdentity::FCanonicalIdentity &OutIdentity,
    FString &OutError) {
  return BuildIdentity(ActorIdentityModelId, TEXT("level_actor"),
                       ActorSelectors(Actor), OutIdentity, OutError);
}

bool BuildComponentIdentity(
    const UActorComponent &Component,
    UeAgentAuthoringIdentity::FCanonicalIdentity &OutIdentity,
    FString &OutError) {
  return BuildIdentity(ComponentIdentityModelId, TEXT("level_component"),
                       ComponentSelectors(Component), OutIdentity, OutError);
}

bool BuildWorldSnapshotReadRequests(
    UWorld *World, const FString &WorldPath, const bool bExpectedAbsent,
    TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
    FString &OutError) {
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
    FString &OutError) {
  OutRequests.Reset();
  OutError.Reset();
  return Identity.IsValid() &&
         AddReadRequest(ActorStateSemanticId, Identity, Actor, bExpectedAbsent,
                        OutRequests);
}

bool BuildComponentSnapshotReadRequests(
    UActorComponent *Component,
    const UeAgentAuthoringIdentity::FCanonicalIdentity &Identity,
    const bool bExpectedAbsent,
    TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
    FString &OutError) {
  OutRequests.Reset();
  OutError.Reset();
  return Identity.IsValid() &&
         AddReadRequest(ComponentStateSemanticId, Identity, Component,
                        bExpectedAbsent, OutRequests);
}

bool BuildWorldExportReadRequests(
    UWorld &World, TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
    FString &OutError) {
  OutRequests.Reset();
  UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
  return BuildWorldIdentity(World, Identity, OutError) &&
         AddReadRequest(WorldTargetSemanticId, Identity, &World, false,
                        OutRequests);
}

bool BuildActorExportReadRequests(
    AActor &Actor, TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
    FString &OutError) {
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
    FString &OutError) {
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
  return AppendEditableProperties(
      &Component, ComponentPropertySemanticId, ComponentPropertyIdentityModelId,
      TEXT("level_component_property"), Selectors,
      {TEXT("AttachParent"), TEXT("AttachSocketName"), TEXT("RelativeLocation"),
       TEXT("RelativeRotation"), TEXT("RelativeScale3D"),
       TEXT("CreationMethod")},
      OutRequests, OutError);
}

bool RegisterStateAccessor(FString &OutError) {
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

bool RegisterNativeDefaultProvider(FString &OutError) {
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
