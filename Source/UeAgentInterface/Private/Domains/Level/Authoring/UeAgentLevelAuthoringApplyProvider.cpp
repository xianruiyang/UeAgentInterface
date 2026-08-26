// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Level/Authoring/UeAgentLevelAuthoringApplyProvider.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Commands/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringAuthorability.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringDescriptors.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringScratchWorld.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringSpatial.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringState.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/PackageName.h"
#include "UObject/StrongObjectPtr.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Utilities/AuthoringOperationApply/UeAgentApplyInputContract.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringObjectApplyProvider.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentApplyWorkspaceContract.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceDependencyGraph.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"

namespace UeAgentLevelAuthoring {
namespace {
using namespace UeAgentApplyInputContract;
using namespace UeAgentAuthoringOperationApply;

void SetFailure(FOperationFailure &OutFailure, const FString &Code,
                const FString &Detail = FString()) {
  OutFailure = FOperationFailure();
  OutFailure.ErrorCode = Code;
  OutFailure.ErrorDetail = Detail.IsEmpty() ? Code : Detail;
}

void SetLookupFailure(FOperationFailure &OutFailure, const TCHAR *FallbackCode,
                      const FString &LookupError,
                      const EAuthorabilityPublicTarget PublicTarget) {
  FAuthorabilityResult Authorability;
  if (TryNormalizeAuthorabilityFailure(LookupError, PublicTarget,
                                       Authorability)) {
    SetFailure(OutFailure, Authorability.ErrorCode, Authorability.ErrorDetail);
    return;
  }
  SetFailure(OutFailure, FallbackCode, LookupError);
}

FString ReadString(const TSharedPtr<const FJsonObject> &Object,
                   const TCHAR *Field) {
  FString Result;
  if (Object.IsValid())
    Object->TryGetStringField(Field, Result);
  Result.TrimStartAndEndInline();
  return Result;
}

TSharedPtr<const FJsonObject>
ReadObject(const TSharedPtr<const FJsonObject> &Object, const TCHAR *Field) {
  const TSharedPtr<FJsonObject> *Result = nullptr;
  return Object.IsValid() && Object->TryGetObjectField(Field, Result) && Result
             ? *Result
             : nullptr;
}

bool BuildActorIdentityFromTarget(
    const TSharedPtr<const FJsonObject> &Target,
    UeAgentAuthoringIdentity::FCanonicalIdentity &OutIdentity,
    FString &OutError) {
  return UeAgentAuthoringIdentity::BuildCanonicalIdentity(
      ActorIdentityModelId, TEXT("level_actor"),
      {
          {TEXT("world_path"), ReadString(Target, TEXT("world_path"))},
          {TEXT("actor_guid"), ReadString(Target, TEXT("actor_guid"))},
          {TEXT("managed_id"), ReadString(Target, TEXT("managed_id"))},
          {TEXT("object_path"), ReadString(Target, TEXT("object_path"))},
          {TEXT("actor_class"), ReadString(Target, TEXT("actor_class"))},
      },
      OutIdentity, OutError);
}

TSharedRef<FJsonObject>
MakeOwnerActorTarget(const TSharedPtr<const FJsonObject> &ComponentTarget) {
  TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
  Result->SetStringField(TEXT("world_path"),
                         ReadString(ComponentTarget, TEXT("world_path")));
  const FString Guid = ReadString(ComponentTarget, TEXT("owner_actor_guid"));
  const FString Managed = ReadString(ComponentTarget, TEXT("owner_managed_id"));
  if (!Guid.IsEmpty())
    Result->SetStringField(TEXT("actor_guid"), Guid);
  if (!Managed.IsEmpty())
    Result->SetStringField(TEXT("managed_id"), Managed);
  return Result;
}

bool BuildComponentIdentityFromTarget(
    const TSharedPtr<const FJsonObject> &Target,
    UeAgentAuthoringIdentity::FCanonicalIdentity &OutIdentity,
    FString &OutError) {
  return UeAgentAuthoringIdentity::BuildCanonicalIdentity(
      ComponentIdentityModelId, TEXT("level_component"),
      {
          {TEXT("world_path"), ReadString(Target, TEXT("world_path"))},
          {TEXT("owner_actor_guid"),
           ReadString(Target, TEXT("owner_actor_guid"))},
          {TEXT("owner_managed_id"),
           ReadString(Target, TEXT("owner_managed_id"))},
          {TEXT("component_name"), ReadString(Target, TEXT("component_name"))},
          {TEXT("component_class"),
           ReadString(Target, TEXT("component_class"))},
      },
      OutIdentity, OutError);
}

bool ParseActorExtension(const TSharedPtr<const FJsonObject> &Extension,
                         TOptional<FString> &OutLabel,
                         TOptional<FString> &OutFolder,
                         TOptional<FTransformPatch> &OutTransform,
                         FString &OutError) {
  OutLabel.Reset();
  OutFolder.Reset();
  OutTransform.Reset();
  OutError.Reset();
  if (!Extension.IsValid())
    return true;
  if (Extension->HasField(TEXT("label"))) {
    FString Value;
    if (!Extension->TryGetStringField(TEXT("label"), Value)) {
      OutError = TEXT("level_actor_label_invalid");
      return false;
    }
    OutLabel = Value;
  }
  if (Extension->HasField(TEXT("folder_path"))) {
    FString Value;
    if (!Extension->TryGetStringField(TEXT("folder_path"), Value)) {
      OutError = TEXT("level_actor_folder_path_invalid");
      return false;
    }
    OutFolder = Value;
  }
  if (Extension->HasField(TEXT("transform"))) {
    const TSharedPtr<const FJsonObject> TransformObject =
        ReadObject(Extension, TEXT("transform"));
    FTransformPatch Patch;
    if (!TransformObject.IsValid() ||
        !ParseTransformPatch(TransformObject, ETransformFrame::ActorWorld,
                             Patch, OutError)) {
      if (OutError.IsEmpty())
        OutError = TEXT("level_actor_transform_invalid");
      return false;
    }
    OutTransform = MoveTemp(Patch);
  }
  return true;
}

struct FPlaceableActorPatch {
  bool bPresent = false;
  FString Name;
  FString Id;
  FString Category;
};

bool ParsePlaceableActorPatch(
    const TSharedPtr<const FJsonObject> &Extension,
    FPlaceableActorPatch &OutPatch, FString &OutError) {
  OutPatch = {};
  OutError.Reset();
  if (!Extension.IsValid() || !Extension->HasField(TEXT("candidate")))
    return true;
  const TSharedPtr<const FJsonObject> Candidate =
      ReadObject(Extension, TEXT("candidate"));
  if (!Candidate.IsValid()) {
    OutError = TEXT("level_actor_candidate_invalid");
    return false;
  }
  OutPatch.bPresent = true;
  Candidate->TryGetStringField(TEXT("name"), OutPatch.Name);
  Candidate->TryGetStringField(TEXT("id"), OutPatch.Id);
  OutPatch.Name.TrimStartAndEndInline();
  OutPatch.Id.TrimStartAndEndInline();
  if (OutPatch.Name.IsEmpty() && OutPatch.Id.IsEmpty()) {
    OutError = TEXT("level_actor_candidate_selector_required");
    return false;
  }
  if (Extension->HasField(TEXT("placement_context"))) {
    const TSharedPtr<const FJsonObject> Context =
        ReadObject(Extension, TEXT("placement_context"));
    if (!Context.IsValid()) {
      OutError = TEXT("level_actor_placement_context_invalid");
      return false;
    }
    Context->TryGetStringField(TEXT("category"), OutPatch.Category);
    OutPatch.Category.TrimStartAndEndInline();
  }
  return true;
}

bool ValidatePlaceableActorAssertions(
    const TSharedPtr<const FJsonObject> &Extension,
    const UeAgentPaletteSearch::FCandidate &Resolved,
    FString &OutError) {
  const TSharedPtr<const FJsonObject> Candidate =
      ReadObject(Extension, TEXT("candidate"));
  if (!Candidate.IsValid()) {
    OutError = TEXT("level_actor_candidate_invalid");
    return false;
  }
  auto Check = [&Candidate, &Resolved, &OutError](
                   const TCHAR *Field, const TCHAR *Selector) -> bool {
    if (!Candidate->HasField(Field))
      return true;
    FString Expected;
    if (!Candidate->TryGetStringField(Field, Expected) ||
        !Expected.TrimStartAndEnd().Equals(
            Resolved.AuthoringSelectors.FindRef(Selector),
            ESearchCase::CaseSensitive)) {
      OutError = FString(TEXT("level_actor_candidate_assertion_mismatch:")) +
                 Field;
      return false;
    }
    return true;
  };
  return Check(TEXT("actor_class"), TEXT("actor_class")) &&
         Check(TEXT("class_path"), TEXT("actor_class")) &&
         Check(TEXT("asset_path"), TEXT("asset_override")) &&
         Check(TEXT("factory_class"), TEXT("actor_factory")) &&
         Check(TEXT("category"), TEXT("placement_category"));
}

struct FAttachmentPatch {
  bool bPresent = false;
  TOptional<FString> ParentComponent;
  TOptional<FString> SocketName;
  TOptional<FTransformPatch> Transform;
};

bool ParseAttachmentExtension(const TSharedPtr<const FJsonObject> &Extension,
                              FAttachmentPatch &OutPatch, FString &OutError) {
  OutPatch = FAttachmentPatch();
  OutError.Reset();
  if (!Extension.IsValid() || !Extension->HasField(TEXT("attachment")))
    return true;
  const TSharedPtr<const FJsonObject> Attachment =
      ReadObject(Extension, TEXT("attachment"));
  if (!Attachment.IsValid()) {
    OutError = TEXT("level_component_attachment_invalid");
    return false;
  }
  OutPatch.bPresent = true;
  if (Attachment->HasField(TEXT("parent_component"))) {
    FString Value;
    if (!Attachment->TryGetStringField(TEXT("parent_component"), Value)) {
      OutError = TEXT("level_component_parent_invalid");
      return false;
    }
    OutPatch.ParentComponent = Value;
  }
  if (Attachment->HasField(TEXT("socket_name"))) {
    FString Value;
    if (!Attachment->TryGetStringField(TEXT("socket_name"), Value)) {
      OutError = TEXT("level_component_socket_invalid");
      return false;
    }
    OutPatch.SocketName = Value;
  }
  if (!OutPatch.ParentComponent.IsSet() || !OutPatch.SocketName.IsSet()) {
    OutError = TEXT("level_component_attachment_patch_incomplete");
    return false;
  }
  if (Attachment->HasField(TEXT("transform"))) {
    const TSharedPtr<const FJsonObject> TransformObject =
        ReadObject(Attachment, TEXT("transform"));
    FTransformPatch Patch;
    if (!TransformObject.IsValid() ||
        !ParseTransformPatch(TransformObject,
                             ETransformFrame::ComponentParentRelative, Patch,
                             OutError)) {
      if (OutError.IsEmpty())
        OutError = TEXT("level_component_transform_invalid");
      return false;
    }
    OutPatch.Transform = MoveTemp(Patch);
  }
  return true;
}

void ApplyActorExtension(AActor &Actor, const TOptional<FString> &Label,
                         const TOptional<FString> &Folder,
                         const TOptional<FTransformPatch> &Transform) {
  if (Label.IsSet())
    Actor.SetActorLabel(Label.GetValue());
  if (Folder.IsSet())
    Actor.SetFolderPath(FName(*Folder.GetValue()));
  if (Transform.IsSet()) {
    FTransform Value = Actor.GetActorTransform();
    ApplyTransformPatch(Transform.GetValue(), Value);
    Actor.SetActorTransform(Value, false, nullptr,
                            ETeleportType::TeleportPhysics);
  }
}

struct FActorAttachmentPatch {
  bool bPresent = false;
  FString ParentManagedId;
  FString SocketName;
};

bool ParseActorAttachmentExtension(
    const TSharedPtr<const FJsonObject> &Extension,
    FActorAttachmentPatch &OutPatch, FString &OutError) {
  OutPatch = FActorAttachmentPatch();
  OutError.Reset();
  if (!Extension.IsValid() || !Extension->HasField(TEXT("attachment")))
    return true;
  const TSharedPtr<const FJsonObject> Attachment =
      ReadObject(Extension, TEXT("attachment"));
  if (!Attachment.IsValid() ||
      !Attachment->TryGetStringField(TEXT("parent_managed_id"),
                                     OutPatch.ParentManagedId)) {
    OutError = TEXT("level_actor_attachment_parent_managed_id_required");
    return false;
  }
  if (Attachment->HasField(TEXT("socket_name")) &&
      !Attachment->TryGetStringField(TEXT("socket_name"),
                                     OutPatch.SocketName)) {
    OutError = TEXT("level_actor_attachment_socket_invalid");
    return false;
  }
  OutPatch.ParentManagedId.TrimStartAndEndInline();
  OutPatch.SocketName.TrimStartAndEndInline();
  OutPatch.bPresent = true;
  return true;
}

bool ApplyActorAttachment(AActor &Actor, UWorld &World,
                          const FActorAttachmentPatch &Patch,
                          FString &OutError) {
  OutError.Reset();
  if (!Patch.bPresent)
    return true;
  if (Patch.ParentManagedId.IsEmpty()) {
    Actor.DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
    return true;
  }
  TSharedRef<FJsonObject> ParentTarget = MakeShared<FJsonObject>();
  ParentTarget->SetStringField(TEXT("world_path"), GetWorldPath(World));
  ParentTarget->SetStringField(TEXT("managed_id"), Patch.ParentManagedId);
  AActor *Parent = FindActorExact(World, ParentTarget, OutError);
  if (!Parent) {
    if (OutError.IsEmpty())
      OutError = TEXT("level_actor_attachment_parent_not_found:") +
                 Patch.ParentManagedId;
    return false;
  }
  if (Parent == &Actor) {
    OutError = TEXT("level_actor_attachment_self_parent");
    return false;
  }
  if (!Actor.GetRootComponent() || !Parent->GetRootComponent()) {
    OutError = TEXT("level_actor_attachment_requires_root_components");
    return false;
  }
  if (!Actor.AttachToActor(
          Parent, FAttachmentTransformRules::KeepWorldTransform,
          Patch.SocketName.IsEmpty() ? NAME_None : FName(*Patch.SocketName))) {
    OutError = TEXT("level_actor_attachment_apply_failed:") +
               Patch.ParentManagedId;
    return false;
  }
  return true;
}

void AddTransformOverrideContext(TMap<FString, FString> &Context,
                                 const TCHAR *Prefix,
                                 const FTransform &Transform) {
  const FString KeyPrefix(Prefix);
  Context.Add(KeyPrefix + TEXT("transform_override"), TEXT("true"));
  Context.Add(KeyPrefix + TEXT("transform_location"),
              Transform.GetLocation().ToString());
  Context.Add(KeyPrefix + TEXT("transform_rotation"),
              Transform.Rotator().ToString());
  Context.Add(KeyPrefix + TEXT("transform_scale"),
              Transform.GetScale3D().ToString());
}

bool ApplyComponentAttachment(UActorComponent &Component, AActor &Owner,
                              const FAttachmentPatch &Patch,
                              FString &OutError) {
  OutError.Reset();
  if (!Patch.bPresent)
    return true;
  USceneComponent *Scene = Cast<USceneComponent>(&Component);
  if (!Scene) {
    OutError = TEXT("level_component_attachment_requires_scene_component");
    return false;
  }

  USceneComponent *Parent = Scene->GetAttachParent();
  if (Patch.ParentComponent.IsSet()) {
    const FString &ParentName = Patch.ParentComponent.GetValue();
    if (ParentName.IsEmpty()) {
      Scene->DetachFromComponent(
          FDetachmentTransformRules::KeepRelativeTransform);
      Parent = nullptr;
    } else {
      FString FindError;
      UActorComponent *Found = FindComponentExact(Owner, ParentName, FindError);
      Parent = Cast<USceneComponent>(Found);
      if (!Parent || Parent == Scene) {
        OutError = FindError.IsEmpty()
                       ? TEXT("level_component_parent_not_found:") + ParentName
                       : FindError;
        return false;
      }
    }
  }
  const FName Socket = Patch.SocketName.IsSet()
                           ? FName(*Patch.SocketName.GetValue())
                           : Scene->GetAttachSocketName();
  if (Parent) {
    if (!Scene->AttachToComponent(
            Parent, FAttachmentTransformRules::KeepRelativeTransform, Socket)) {
      OutError = TEXT("level_component_attach_failed:") + Parent->GetName();
      return false;
    }
  }
  if (Patch.Transform.IsSet()) {
    FTransform Value = Scene->GetRelativeTransform();
    ApplyTransformPatch(Patch.Transform.GetValue(), Value);
    Scene->SetRelativeTransform(Value, false, nullptr,
                                ETeleportType::TeleportPhysics);
  }
  return true;
}

TSharedRef<FJsonObject> BuildCounterMetadata(const TCHAR *Field,
                                             const int32 Value = 1) {
  TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
  Result->SetNumberField(Field, Value);
  return Result;
}

class FWorldLifecycleState final : public IReflectionOperationLifecycleState {
public:
  FWorldLifecycleState(UWorld &InWorld,
                       UeAgentAuthoringIdentity::FCanonicalIdentity InIdentity)
      : World(&InWorld), Identity(MoveTemp(InIdentity)) {}

  virtual FString GetTargetKey() const override {
    return Identity.CanonicalKey;
  }
  virtual EOperation GetResolvedOperation() const override {
    return EOperation::Update;
  }
  virtual FReflectionRoot GetProposedRoot() const override {
    return FReflectionRoot::MakeObject(World.Get());
  }
  virtual FReflectionRoot GetActualRoot() const override {
    return FReflectionRoot::MakeObject(World.Get());
  }
  virtual bool CaptureRollback(FOperationFailure &OutFailure) override {
    OutFailure = {};
    return true;
  }
  virtual bool ApplyLifecycle(FOperationFailure &OutFailure) override {
    OutFailure = {};
    return true;
  }
  virtual bool Compile(FOperationFailure &OutFailure) override {
    OutFailure = {};
    return true;
  }
  virtual bool Save(FOperationFailure &OutFailure) override {
    OutFailure = {};
    return true;
  }
  virtual bool BuildDomainExpectedReadRequests(
      TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
      FOperationFailure &OutFailure) const override {
    return BuildRequests(OutRequests, OutFailure);
  }
  virtual bool BuildDomainActualReadRequests(
      TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
      FOperationFailure &OutFailure) const override {
    return BuildRequests(OutRequests, OutFailure);
  }
  virtual bool Rollback(FOperationFailure &OutFailure) override {
    OutFailure = {};
    return true;
  }

private:
  bool BuildRequests(TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
                     FOperationFailure &OutFailure) const {
    FString Error;
    if (!BuildWorldSnapshotReadRequests(
            World.Get(), Identity.GetSelector(TEXT("world_path")), false,
            OutRequests, Error)) {
      SetFailure(OutFailure, TEXT("level_world_snapshot_failed"), Error);
      return false;
    }
    OutFailure = {};
    return true;
  }

  TWeakObjectPtr<UWorld> World;
  UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
};

class FActorLifecycleState final : public IReflectionOperationLifecycleState {
public:
  FActorLifecycleState(UWorld &InWorld,
                       TSharedRef<FLevelAuthoringScratchWorld> InScratchWorld,
                       EOperation InOperation,
                       UeAgentAuthoringIdentity::FCanonicalIdentity InIdentity,
                       AActor *InExisting, AActor *InProposed,
                       FString InBaseLabel, FString InBaseFolder,
                       FTransform InBaseTransform, TOptional<FString> InLabel,
                       TOptional<FString> InFolder,
                       TOptional<FTransformPatch> InTransform,
	                       FActorAttachmentPatch InAttachment,
	                       FPlaceableActorPatch InPlaceableCandidate)
      : World(&InWorld), ScratchWorld(MoveTemp(InScratchWorld)),
        Operation(InOperation), Identity(MoveTemp(InIdentity)),
        Existing(InExisting), Proposed(InProposed),
        BaseLabel(MoveTemp(InBaseLabel)), BaseFolder(MoveTemp(InBaseFolder)),
        BaseTransform(MoveTemp(InBaseTransform)), Label(MoveTemp(InLabel)),
        Folder(MoveTemp(InFolder)), Transform(MoveTemp(InTransform)),
	        Attachment(MoveTemp(InAttachment)),
	        PlaceableCandidate(MoveTemp(InPlaceableCandidate)) {}

  virtual FString GetTargetKey() const override {
    return Identity.CanonicalKey;
  }
  virtual EOperation GetResolvedOperation() const override { return Operation; }
  virtual FReflectionRoot GetProposedRoot() const override {
    return FReflectionRoot::MakeObject(
        Operation == EOperation::Remove ? Existing.Get() : Proposed.Get());
  }
  virtual FReflectionRoot GetActualRoot() const override {
    return FReflectionRoot::MakeObject(
        Operation == EOperation::Remove ? nullptr
                                        : (Actual ? Actual : Existing.Get()));
  }
  virtual bool ShouldApplyPropertiesToActualRoot() const override {
    return true;
  }

  virtual bool CaptureRollback(FOperationFailure &OutFailure) override {
    OutFailure = {};
    if (!Existing.IsValid())
      return true;
    FString Error;
    Backup.Reset(ScratchWorld->DuplicateActor(*Existing, Error));
    if (!Backup.IsValid()) {
      SetFailure(OutFailure, TEXT("level_actor_rollback_capture_failed"),
                 Error);
      return false;
    }
    OriginalGuid = Existing->GetActorGuid();
    OriginalName = Existing->GetFName();
    OriginalAttachParent = Existing->GetAttachParentActor();
    OriginalAttachSocket = Existing->GetAttachParentSocketName();
    return true;
  }

  virtual bool ApplyProposed(FOperationFailure &OutFailure) override {
    OutFailure = {};
    if (Operation == EOperation::Remove)
      return true;
    if (!Proposed.IsValid()) {
      SetFailure(OutFailure, TEXT("level_actor_proposed_missing"),
                 Identity.CanonicalKey);
      return false;
    }
    return true;
  }

  virtual bool ApplyLifecycle(FOperationFailure &OutFailure) override {
    OutFailure = {};
    if (!World.IsValid()) {
      SetFailure(OutFailure, TEXT("level_world_not_found"));
      return false;
    }
    if (Operation == EOperation::Remove) {
      if (!Existing.IsValid() ||
          !World->DestroyActor(Existing.Get(), true, true)) {
        SetFailure(OutFailure, TEXT("level_actor_remove_failed"),
                   Identity.CanonicalKey);
        return false;
      }
      Existing.Reset();
      return true;
    }
    if (Operation == EOperation::Update) {
      Actual = Existing.Get();
      if (!Actual) {
        SetFailure(OutFailure, TEXT("level_actor_not_found"),
                   Identity.CanonicalKey);
        return false;
      }
      Actual->Modify();
      ApplyActorExtension(*Actual, Label, Folder, Transform);
    } else {
      FActorSpawnParameters Params;
      Params.Template = Proposed.Get();
      Params.OverrideLevel = World->PersistentLevel;
      Params.SpawnCollisionHandlingOverride =
          ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
      FString GuidText = Identity.GetSelector(TEXT("actor_guid"));
      FGuid DesiredGuid;
      if (!GuidText.IsEmpty() && FGuid::Parse(GuidText, DesiredGuid))
        Params.OverrideActorGuid = DesiredGuid;
	      if (PlaceableCandidate.bPresent) {
	        if (!World->PersistentLevel) {
	          SetFailure(OutFailure, TEXT("level_actor_persistent_level_missing"));
	          return false;
	        }
	        UeAgentPaletteSearch::FCandidate ResolvedCandidate;
	        FString CandidateError;
	        if (!UeAgentPaletteSearch::TryPerformPlaceableActorCandidate(
	                PlaceableCandidate.Name, PlaceableCandidate.Id,
	                PlaceableCandidate.Category, *World->PersistentLevel,
	                Proposed->GetActorTransform(), Params, Actual,
	                ResolvedCandidate, CandidateError)) {
	          SetFailure(OutFailure,
	                     TEXT("level_actor_native_candidate_create_failed"),
	                     CandidateError);
	          return false;
	        }
	      } else {
	        Actual = World->SpawnActor<AActor>(Proposed->GetClass(),
	                                           Proposed->GetActorTransform(), Params);
	      }
      if (!Actual) {
        SetFailure(OutFailure, TEXT("level_actor_spawn_failed"),
                   Identity.CanonicalKey);
        return false;
      }
      const FString ManagedId = Identity.GetSelector(TEXT("managed_id"));
      if (!ManagedId.IsEmpty())
        Actual->Tags.AddUnique(FName(*MakeManagedActorTag(ManagedId)));
      ApplyActorExtension(*Actual, Label, Folder, Transform);
    }
    FString AttachmentError;
    if (!ApplyActorAttachment(*Actual, *World, Attachment, AttachmentError)) {
      SetLookupFailure(OutFailure, TEXT("level_actor_attachment_apply_failed"),
                       AttachmentError, EAuthorabilityPublicTarget::Actor);
      return false;
    }
    Actual->PostEditChange();
    Actual->MarkPackageDirty();
    return true;
  }

  virtual bool Compile(FOperationFailure &OutFailure) override {
    OutFailure = {};
    return true;
  }
  virtual bool Save(FOperationFailure &OutFailure) override {
    OutFailure = {};
    return true;
  }
  virtual bool BuildDomainExpectedReadRequests(
      TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
      FOperationFailure &OutFailure) const override {
    if (!BuildRequests(
            Operation == EOperation::Remove ? nullptr : Proposed.Get(),
            Operation == EOperation::Remove, OutRequests, OutFailure)) {
      return false;
    }
    if (Operation != EOperation::Remove && !OutRequests.IsEmpty()) {
      TMap<FString, FString> &Context = OutRequests[0].Context;
      const bool bOmitGeneratedLabel =
          (Operation == EOperation::Create || Operation == EOperation::Clone) &&
          !Label.IsSet();
      if (bOmitGeneratedLabel) {
        Context.Add(TEXT("actor_label_omit"), TEXT("true"));
      } else {
        Context.Add(TEXT("actor_label_override"),
                    Label.IsSet() ? Label.GetValue() : BaseLabel);
      }
      Context.Add(TEXT("actor_folder_override"),
                  Folder.IsSet() ? Folder.GetValue() : BaseFolder);
      FTransform Value = BaseTransform;
      if (Transform.IsSet())
        ApplyTransformPatch(Transform.GetValue(), Value);
      AddTransformOverrideContext(Context, TEXT("actor_"), Value);
      if (Attachment.bPresent) {
        Context.Add(TEXT("actor_attachment_override"), TEXT("true"));
        Context.Add(TEXT("actor_parent_managed_id_override"),
                    Attachment.ParentManagedId);
        Context.Add(TEXT("actor_socket_name_override"),
                    Attachment.SocketName);
      }
    }
    return true;
  }
  virtual bool BuildDomainActualReadRequests(
      TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
      FOperationFailure &OutFailure) const override {
    if (!BuildRequests(Operation == EOperation::Remove
                           ? nullptr
                           : (Actual ? Actual : Existing.Get()),
                       Operation == EOperation::Remove, OutRequests,
                       OutFailure)) {
      return false;
    }
    if ((Operation == EOperation::Create || Operation == EOperation::Clone) &&
        !Label.IsSet() && !OutRequests.IsEmpty()) {
      OutRequests[0].Context.Add(TEXT("actor_label_omit"), TEXT("true"));
    }
    return true;
  }
  virtual TSharedPtr<FJsonObject> BuildResultMetadata() const override {
    if (Operation == EOperation::Remove)
      return BuildCounterMetadata(TEXT("actors_removed"));
    return BuildCounterMetadata(Operation == EOperation::Update
                                    ? TEXT("actors_updated")
                                    : TEXT("actors_created"));
  }
  virtual bool Rollback(FOperationFailure &OutFailure) override {
    OutFailure = {};
    if (!World.IsValid()) {
      SetFailure(OutFailure, TEXT("level_world_not_found"));
      return false;
    }
    if (Operation == EOperation::Create || Operation == EOperation::Clone) {
      if (Actual && !World->DestroyActor(Actual, true, true)) {
        SetFailure(OutFailure, TEXT("level_actor_create_rollback_failed"),
                   Identity.CanonicalKey);
        return false;
      }
      Actual = nullptr;
      return true;
    }
    if (Operation == EOperation::Update && Existing.IsValid() &&
        Backup.IsValid()) {
      ApplyActorExtension(*Existing, BaseLabel, BaseFolder,
                          FTransformPatch{BaseTransform.GetLocation(),
                                          BaseTransform.Rotator(),
                                          BaseTransform.GetScale3D()});
      Existing->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
      if (OriginalAttachParent.IsValid()) {
        Existing->AttachToActor(
            OriginalAttachParent.Get(),
            FAttachmentTransformRules::KeepWorldTransform,
            OriginalAttachSocket);
      }
      Actual = Existing.Get();
      return true;
    }
    if (Operation == EOperation::Remove && Backup.IsValid()) {
      FActorSpawnParameters Params;
      Params.Template = Backup.Get();
      Params.OverrideLevel = World->PersistentLevel;
      Params.OverrideActorGuid = OriginalGuid;
      Params.Name = OriginalName;
      Params.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;
      Params.SpawnCollisionHandlingOverride =
          ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
      Actual = World->SpawnActor<AActor>(Backup->GetClass(),
                                         Backup->GetActorTransform(), Params);
      if (!Actual) {
        SetFailure(OutFailure, TEXT("level_actor_remove_rollback_failed"),
                   Identity.CanonicalKey);
        return false;
      }
      Existing.Reset(Actual);
      ApplyActorExtension(*Actual, BaseLabel, BaseFolder,
                          FTransformPatch{BaseTransform.GetLocation(),
                                          BaseTransform.Rotator(),
                                          BaseTransform.GetScale3D()});
      if (OriginalAttachParent.IsValid()) {
        Actual->AttachToActor(
            OriginalAttachParent.Get(),
            FAttachmentTransformRules::KeepWorldTransform,
            OriginalAttachSocket);
      }
    }
    return true;
  }

private:
  bool BuildRequests(AActor *Actor, const bool bExpectedAbsent,
                     TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
                     FOperationFailure &OutFailure) const {
    FString Error;
    if (!BuildActorSnapshotReadRequests(Actor, Identity, bExpectedAbsent,
                                        OutRequests, Error)) {
      SetFailure(OutFailure, TEXT("level_actor_snapshot_failed"), Error);
      return false;
    }
    OutFailure = {};
    return true;
  }

  TWeakObjectPtr<UWorld> World;
  TSharedRef<FLevelAuthoringScratchWorld> ScratchWorld;
  EOperation Operation = EOperation::Update;
  UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
  TStrongObjectPtr<AActor> Existing;
  TStrongObjectPtr<AActor> Proposed;
  TStrongObjectPtr<AActor> Backup;
  AActor *Actual = nullptr;
  FGuid OriginalGuid;
  FName OriginalName;
  TWeakObjectPtr<AActor> OriginalAttachParent;
  FName OriginalAttachSocket;
  FString BaseLabel;
  FString BaseFolder;
  FTransform BaseTransform = FTransform::Identity;
  TOptional<FString> Label;
  TOptional<FString> Folder;
  TOptional<FTransformPatch> Transform;
  FActorAttachmentPatch Attachment;
	  FPlaceableActorPatch PlaceableCandidate;
};

class FComponentLifecycleState final
    : public IReflectionOperationLifecycleState {
public:
  FComponentLifecycleState(
      UWorld &InWorld, TSharedRef<FLevelAuthoringScratchWorld> InScratchWorld,
      EOperation InOperation, bool bInResolveUpsertAgainstSpawnedOwner,
      UeAgentAuthoringIdentity::FCanonicalIdentity InIdentity,
      TSharedPtr<const FJsonObject> InOwnerTarget, UActorComponent *InExisting,
      UActorComponent *InProposed, FAttachmentPatch InAttachment)
      : World(&InWorld), ScratchWorld(MoveTemp(InScratchWorld)),
        Operation(InOperation),
        bResolveUpsertAgainstSpawnedOwner(bInResolveUpsertAgainstSpawnedOwner),
        Identity(MoveTemp(InIdentity)), OwnerTarget(MoveTemp(InOwnerTarget)),
        Existing(InExisting), Proposed(InProposed),
        Attachment(MoveTemp(InAttachment)) {}

  virtual FString GetTargetKey() const override {
    return Identity.CanonicalKey;
  }
  virtual EOperation GetResolvedOperation() const override { return Operation; }
  virtual FReflectionRoot GetProposedRoot() const override {
    return FReflectionRoot::MakeObject(
        Operation == EOperation::Remove ? Existing.Get() : Proposed.Get());
  }
  virtual FReflectionRoot GetActualRoot() const override {
    return FReflectionRoot::MakeObject(
        Operation == EOperation::Remove ? nullptr
                                        : (Actual ? Actual : Existing.Get()));
  }
  virtual bool ShouldApplyPropertiesToActualRoot() const override {
    return true;
  }

  virtual bool CaptureRollback(FOperationFailure &OutFailure) override {
    OutFailure = {};
    if (!Existing.IsValid())
      return true;
    FString Error;
    Backup.Reset(ScratchWorld->DuplicateComponent(*Existing, Error));
    if (!Backup.IsValid()) {
      SetFailure(OutFailure, TEXT("level_component_rollback_capture_failed"),
                 Error);
      return false;
    }
    OriginalName = Existing->GetFName();
    if (const USceneComponent *Scene = Cast<USceneComponent>(Existing.Get())) {
      bHadSceneState = true;
      const AActor *Owner = Existing->GetOwner();
      bWasRootComponent = Owner && Owner->GetRootComponent() == Scene;
      OriginalParentName = Scene->GetAttachParent()
                               ? Scene->GetAttachParent()->GetName()
                               : FString();
      OriginalSocketName = Scene->GetAttachSocketName().IsNone()
                               ? FString()
                               : Scene->GetAttachSocketName().ToString();
      OriginalRelativeTransform = Scene->GetRelativeTransform();
    }
    return true;
  }

  virtual bool ApplyProposed(FOperationFailure &OutFailure) override {
    OutFailure = {};
    if (Operation == EOperation::Remove)
      return true;
    if (!Proposed.IsValid()) {
      SetFailure(OutFailure, TEXT("level_component_proposed_missing"),
                 Identity.CanonicalKey);
      return false;
    }
    if (Attachment.Transform.IsSet()) {
      if (!Cast<USceneComponent>(Proposed.Get())) {
        SetFailure(OutFailure,
                   TEXT("level_component_transform_requires_scene_component"));
        return false;
      }
    }
    return true;
  }

  virtual bool ApplyLifecycle(FOperationFailure &OutFailure) override {
    OutFailure = {};
    if (!World.IsValid()) {
      SetFailure(OutFailure, TEXT("level_world_not_found"));
      return false;
    }
    FString OwnerError;
    AActor *Owner = FindActorExact(*World, OwnerTarget, OwnerError);
    if (!Owner) {
      SetLookupFailure(OutFailure, TEXT("level_component_owner_not_found"),
                       OwnerError, EAuthorabilityPublicTarget::Component);
      return false;
    }
    if (Operation == EOperation::Remove) {
      if (!Existing.IsValid()) {
        FString FindError;
        Existing.Reset(FindComponentExact(
            *Owner, Identity.GetSelector(TEXT("component_name")), FindError));
        if (!Existing.IsValid() && !FindError.IsEmpty()) {
          SetLookupFailure(OutFailure, TEXT("level_component_identity_invalid"),
                           FindError, EAuthorabilityPublicTarget::Component);
          return false;
        }
      }
      if (!Existing.IsValid()) {
        SetFailure(OutFailure, TEXT("level_component_not_found"),
                   Identity.CanonicalKey);
        return false;
      }
      const FName RetiredName = MakeUniqueObjectName(
          Owner, Existing->GetClass(),
          FName(*(OriginalName.ToString() + TEXT("_UAI_Removed"))));
      if (!Existing->Rename(
              *RetiredName.ToString(), Owner,
              REN_DontCreateRedirectors | REN_DoNotDirty |
                  REN_NonTransactional)) {
        SetFailure(OutFailure, TEXT("level_component_remove_failed"),
                   TEXT("component_retire_rename_failed"));
        return false;
      }
      Existing->Modify();
      Existing->DestroyComponent();
      Existing.Reset();
      return true;
    }

    FString FindError;
    UActorComponent *Current = FindComponentExact(
        *Owner, Identity.GetSelector(TEXT("component_name")), FindError);
    if (!FindError.IsEmpty()) {
      SetLookupFailure(OutFailure, TEXT("level_component_identity_invalid"),
                       FindError, EAuthorabilityPublicTarget::Component);
      return false;
    }
    if (Operation == EOperation::Create && Current &&
        bResolveUpsertAgainstSpawnedOwner) {
      if (Current->GetClass() != Proposed->GetClass()) {
        SetFailure(OutFailure, TEXT("level_component_class_conflict"),
                   Current->GetClass()->GetPathName());
        return false;
      }
      Operation = EOperation::Update;
      Existing.Reset(Current);
      Actual = Current;
      Actual->Modify();
    } else if (Operation == EOperation::Update) {
      Actual = Current ? Current : Existing.Get();
      if (!Actual) {
        SetFailure(OutFailure, TEXT("level_component_not_found"),
                   Identity.CanonicalKey);
        return false;
      }
      Actual->Modify();
    } else {
      if (Current) {
        SetFailure(OutFailure, TEXT("level_component_already_exists"),
                   Identity.CanonicalKey);
        return false;
      }
      Actual = NewObject<UActorComponent>(
          Owner, Proposed->GetClass(),
          FName(*Identity.GetSelector(TEXT("component_name"))),
          RF_Transactional, Proposed.Get());
      if (!Actual) {
        SetFailure(OutFailure, TEXT("level_component_create_failed"),
                   Identity.CanonicalKey);
        return false;
      }
      Owner->AddOwnedComponent(Actual);
      Owner->AddInstanceComponent(Actual);
      Actual->OnComponentCreated();
      Actual->RegisterComponent();
      if (USceneComponent *Scene = Cast<USceneComponent>(Actual)) {
        if (!Owner->GetRootComponent())
          Owner->SetRootComponent(Scene);
      }
    }
    FString AttachmentError;
    if (!ApplyComponentAttachment(*Actual, *Owner, Attachment,
                                  AttachmentError)) {
      SetLookupFailure(OutFailure,
                       TEXT("level_component_attachment_apply_failed"),
                       AttachmentError, EAuthorabilityPublicTarget::Component);
      return false;
    }
    Actual->PostEditChange();
    Actual->MarkPackageDirty();
    return true;
  }

  virtual bool Compile(FOperationFailure &OutFailure) override {
    OutFailure = {};
    return true;
  }
  virtual bool Save(FOperationFailure &OutFailure) override {
    OutFailure = {};
    return true;
  }
  virtual bool BuildDomainExpectedReadRequests(
      TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
      FOperationFailure &OutFailure) const override {
    if (!BuildRequests(
            Operation == EOperation::Remove ? nullptr : Proposed.Get(),
            Operation == EOperation::Remove, OutRequests, OutFailure)) {
      return false;
    }
    if (Attachment.bPresent && !OutRequests.IsEmpty()) {
      OutRequests[0].Context.Add(TEXT("attachment_override"), TEXT("true"));
      if (Attachment.ParentComponent.IsSet()) {
        OutRequests[0].Context.Add(TEXT("parent_component_override"),
                                   Attachment.ParentComponent.GetValue());
      }
      if (Attachment.SocketName.IsSet()) {
        OutRequests[0].Context.Add(TEXT("socket_name_override"),
                                   Attachment.SocketName.GetValue());
      }
      if (Attachment.Transform.IsSet()) {
        const USceneComponent *Scene =
            CastChecked<USceneComponent>(Proposed.Get());
        FTransform Value = Scene->GetRelativeTransform();
        ApplyTransformPatch(Attachment.Transform.GetValue(), Value);
        AddTransformOverrideContext(OutRequests[0].Context, TEXT("component_"),
                                    Value);
      }
    }
    return true;
  }
  virtual bool BuildDomainActualReadRequests(
      TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
      FOperationFailure &OutFailure) const override {
    return BuildRequests(
        Operation == EOperation::Remove ? nullptr
                                        : (Actual ? Actual : Existing.Get()),
        Operation == EOperation::Remove, OutRequests, OutFailure);
  }
  virtual TSharedPtr<FJsonObject> BuildResultMetadata() const override {
    if (Operation == EOperation::Remove)
      return BuildCounterMetadata(TEXT("components_removed"));
    return BuildCounterMetadata(Operation == EOperation::Update
                                    ? TEXT("components_updated")
                                    : TEXT("components_created"));
  }
  virtual bool Rollback(FOperationFailure &OutFailure) override {
    OutFailure = {};
    if (!World.IsValid()) {
      SetFailure(OutFailure, TEXT("level_world_not_found"));
      return false;
    }
    FString OwnerError;
    AActor *Owner = FindActorExact(*World, OwnerTarget, OwnerError);
    if (!Owner) {
      SetLookupFailure(OutFailure, TEXT("level_component_owner_not_found"),
                       OwnerError, EAuthorabilityPublicTarget::Component);
      return false;
    }
    if (Operation == EOperation::Create || Operation == EOperation::Clone) {
      if (Actual)
        Actual->DestroyComponent();
      Actual = nullptr;
      return true;
    }
    if (Operation == EOperation::Update && Existing.IsValid() &&
        Backup.IsValid()) {
      Actual = Existing.Get();
      if (bHadSceneState) {
        FAttachmentPatch Restore;
        Restore.bPresent = true;
        Restore.ParentComponent = OriginalParentName;
        Restore.SocketName = OriginalSocketName;
        Restore.Transform =
            FTransformPatch{OriginalRelativeTransform.GetLocation(),
                            OriginalRelativeTransform.Rotator(),
                            OriginalRelativeTransform.GetScale3D()};
        FString RestoreError;
        if (!ApplyComponentAttachment(*Actual, *Owner, Restore, RestoreError)) {
          SetLookupFailure(OutFailure,
                           TEXT("level_component_update_rollback_failed"),
                           RestoreError, EAuthorabilityPublicTarget::Component);
          return false;
        }
      }
      return true;
    }
    if (Operation == EOperation::Remove && Backup.IsValid()) {
      Actual =
          NewObject<UActorComponent>(Owner, Backup->GetClass(), OriginalName,
                                     RF_Transactional, Backup.Get());
      if (!Actual) {
        SetFailure(OutFailure, TEXT("level_component_remove_rollback_failed"),
                   Identity.CanonicalKey);
        return false;
      }
      Owner->AddOwnedComponent(Actual);
      Owner->AddInstanceComponent(Actual);
      Actual->OnComponentCreated();
      Actual->RegisterComponent();
      if (bHadSceneState) {
        FAttachmentPatch Restore;
        Restore.bPresent = true;
        Restore.ParentComponent = OriginalParentName;
        Restore.SocketName = OriginalSocketName;
        Restore.Transform =
            FTransformPatch{OriginalRelativeTransform.GetLocation(),
                            OriginalRelativeTransform.Rotator(),
                            OriginalRelativeTransform.GetScale3D()};
        FString RestoreError;
        if (!ApplyComponentAttachment(*Actual, *Owner, Restore, RestoreError)) {
          SetLookupFailure(OutFailure,
                           TEXT("level_component_remove_rollback_failed"),
                           RestoreError, EAuthorabilityPublicTarget::Component);
          return false;
        }
      }
      if (bWasRootComponent) {
        USceneComponent *RestoredRoot = Cast<USceneComponent>(Actual);
        if (!RestoredRoot || !Owner->SetRootComponent(RestoredRoot)) {
          SetFailure(OutFailure,
                     TEXT("level_component_remove_rollback_failed"),
                     TEXT("root_component_restore_failed"));
          return false;
        }
      }
      Existing.Reset(Actual);
    }
    return true;
  }

private:
  bool BuildRequests(UActorComponent *Component, const bool bExpectedAbsent,
                     TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
                     FOperationFailure &OutFailure) const {
    FString Error;
    if (!BuildComponentSnapshotReadRequests(
            Component, Identity, bExpectedAbsent, OutRequests, Error)) {
      SetFailure(OutFailure, TEXT("level_component_snapshot_failed"), Error);
      return false;
    }
    OutFailure = {};
    return true;
  }

  TWeakObjectPtr<UWorld> World;
  TSharedRef<FLevelAuthoringScratchWorld> ScratchWorld;
  EOperation Operation = EOperation::Update;
  bool bResolveUpsertAgainstSpawnedOwner = false;
  UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
  TSharedPtr<const FJsonObject> OwnerTarget;
  TStrongObjectPtr<UActorComponent> Existing;
  TStrongObjectPtr<UActorComponent> Proposed;
  TStrongObjectPtr<UActorComponent> Backup;
  UActorComponent *Actual = nullptr;
  FName OriginalName;
  bool bHadSceneState = false;
  bool bWasRootComponent = false;
  FString OriginalParentName;
  FString OriginalSocketName;
  FTransform OriginalRelativeTransform = FTransform::Identity;
  FAttachmentPatch Attachment;
};

class FLevelLifecycle final : public IReflectionOperationLifecycle {
public:
  FLevelLifecycle() = default;

  explicit FLevelLifecycle(UWorld &InBoundWorld)
      : BoundWorld(&InBoundWorld),
        SharedScratchWorld(MakeShared<FLevelAuthoringScratchWorld>()),
        bHasBoundWorld(true) {}

  virtual bool
  PrepareLifecycle(const FPrepareRequest &Request,
                   TUniquePtr<IReflectionOperationLifecycleState> &OutState,
                   FOperationFailure &OutFailure) const override {
    OutState.Reset();
    OutFailure = {};
    if (!Request.IsValid() || !Request.Input->Target.IsValid()) {
      SetFailure(OutFailure, TEXT("level_apply_request_invalid"));
      return false;
    }
    const FString WorldPath =
        ReadString(Request.Input->Target, TEXT("world_path"));
    UWorld *World = ResolveWorld(WorldPath);
    if (!World) {
      SetFailure(OutFailure, TEXT("level_world_not_found"), WorldPath);
      return false;
    }

    if (Request.Contract->ContractId == WorldInputContractId) {
      UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
      FString Error;
      if (Request.Input->RequestedOperation != EOperation::Update ||
          !BuildWorldIdentity(*World, Identity, Error)) {
        SetFailure(OutFailure, TEXT("level_world_operation_invalid"), Error);
        return false;
      }
      OutState = MakeUnique<FWorldLifecycleState>(*World, MoveTemp(Identity));
      return true;
    }

    if (Request.Contract->ContractId == ActorInputContractId) {
      return PrepareActor(*World, Request, OutState, OutFailure);
    }
    if (Request.Contract->ContractId == ComponentInputContractId) {
      return PrepareComponent(*World, Request, OutState, OutFailure);
    }
    SetFailure(OutFailure, TEXT("level_apply_contract_unsupported"),
               Request.Contract->ContractId);
    return false;
  }

private:
  TSharedRef<FLevelAuthoringScratchWorld> GetScratchWorld() const {
    return SharedScratchWorld.IsValid()
               ? SharedScratchWorld.ToSharedRef()
               : MakeShared<FLevelAuthoringScratchWorld>();
  }

  UWorld *ResolveWorld(const FString &WorldPath) const {
    if (!bHasBoundWorld)
      return FindEditorWorldByPath(WorldPath);
    UWorld *World = BoundWorld.Get();
    return World && GetWorldPath(*World).Equals(WorldPath,
                                                ESearchCase::IgnoreCase)
               ? World
               : nullptr;
  }

  bool PrepareActor(UWorld &World, const FPrepareRequest &Request,
                    TUniquePtr<IReflectionOperationLifecycleState> &OutState,
                    FOperationFailure &OutFailure) const {
    FString Error;
    UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
    if (!BuildActorIdentityFromTarget(Request.Input->Target, Identity, Error)) {
      SetFailure(OutFailure, TEXT("level_actor_identity_invalid"), Error);
      return false;
    }
    AActor *Existing = FindActorExact(World, Request.Input->Target, Error);
    if (!Error.IsEmpty()) {
      SetLookupFailure(OutFailure, TEXT("level_actor_identity_invalid"), Error,
                       EAuthorabilityPublicTarget::Actor);
      return false;
    }
    EOperation Operation = Request.Input->RequestedOperation;
    if (Operation == EOperation::Upsert)
      Operation = Existing ? EOperation::Update : EOperation::Create;
    if ((Operation == EOperation::Create || Operation == EOperation::Clone) &&
        Existing) {
      SetFailure(OutFailure, TEXT("level_actor_already_exists"),
                 Identity.CanonicalKey);
      return false;
    }
    if ((Operation == EOperation::Update || Operation == EOperation::Remove) &&
        !Existing) {
      SetFailure(OutFailure, TEXT("level_actor_not_found"),
                 Identity.CanonicalKey);
      return false;
    }

	    FPlaceableActorPatch PlaceableCandidate;
	    FString CandidateActorClass;
	    if (!ParsePlaceableActorPatch(Request.Input->ProviderExtension,
	                                  PlaceableCandidate, Error)) {
	      SetFailure(OutFailure, TEXT("level_actor_candidate_invalid"), Error);
	      return false;
	    }
	    if (PlaceableCandidate.bPresent) {
	      if (Operation == EOperation::Clone || Operation == EOperation::Remove) {
	        SetFailure(OutFailure,
	                   TEXT("level_actor_candidate_operation_unsupported"));
	        return false;
	      }
	      UeAgentPaletteSearch::FCandidate ResolvedCandidate;
	      if (!UeAgentPaletteSearch::TryResolvePlaceableActorCandidate(
	              PlaceableCandidate.Name, PlaceableCandidate.Id,
	              PlaceableCandidate.Category, ResolvedCandidate, Error)) {
	        SetFailure(OutFailure, TEXT("level_actor_candidate_unresolved"),
	                   Error);
	        return false;
	      }
	      const FString ResolvedClass =
	          ResolvedCandidate.AuthoringSelectors.FindRef(TEXT("actor_class"));
	      CandidateActorClass = ResolvedClass;
	      const FString AssertedClass =
	          ReadString(Request.Input->Target, TEXT("actor_class"));
	      if (ResolvedClass.IsEmpty() ||
	          (!AssertedClass.IsEmpty() && AssertedClass != ResolvedClass)) {
	        SetFailure(OutFailure, TEXT("level_actor_candidate_class_mismatch"),
	                   AssertedClass);
	        return false;
	      }
	      if (!ValidatePlaceableActorAssertions(Request.Input->ProviderExtension,
	                                             ResolvedCandidate, Error)) {
	        SetFailure(OutFailure,
	                   TEXT("level_actor_candidate_assertion_mismatch"), Error);
	        return false;
	      }
	      if (Operation == EOperation::Create &&
	          !ResolvedCandidate.bSupportedInContext) {
	        SetFailure(OutFailure,
	                   TEXT("level_actor_candidate_not_headless_creatable"),
	                   ResolvedCandidate.SupportReason);
	        return false;
	      }
	    }

    AActor *Template = Existing;
    if (Operation == EOperation::Create) {
	      const FString ClassPath = CandidateActorClass.IsEmpty()
	          ? ReadString(Request.Input->Target, TEXT("actor_class"))
	          : CandidateActorClass;
      UClass *Class =
          StaticLoadClass(AActor::StaticClass(), nullptr, *ClassPath);
      if (Class) {
        const FAuthorabilityResult ClassAuthorability =
            EvaluateActorClassAuthorability(*Class);
        if (!ClassAuthorability.bAuthorable) {
          SetFailure(OutFailure, ClassAuthorability.ErrorCode,
                     ClassAuthorability.ErrorDetail);
          return false;
        }
      }
      Template = Class ? Class->GetDefaultObject<AActor>() : nullptr;
      if (!Template) {
        SetFailure(OutFailure, TEXT("level_actor_class_invalid"), ClassPath);
        return false;
      }
    } else if (Operation == EOperation::Clone) {
      TSharedRef<FJsonObject> SourceTarget = MakeShared<FJsonObject>();
      SourceTarget->SetStringField(TEXT("world_path"), GetWorldPath(World));
      const FString SourceGuid =
          ReadString(Request.Input->Target, TEXT("source_actor_guid"));
      const FString SourceManaged =
          ReadString(Request.Input->Target, TEXT("source_managed_id"));
      if (!SourceGuid.IsEmpty())
        SourceTarget->SetStringField(TEXT("actor_guid"), SourceGuid);
      if (!SourceManaged.IsEmpty())
        SourceTarget->SetStringField(TEXT("managed_id"), SourceManaged);
      Template = FindActorExact(World, SourceTarget, Error);
      if (!Template) {
        SetLookupFailure(OutFailure, TEXT("level_actor_clone_source_not_found"),
                         Error, EAuthorabilityPublicTarget::Actor);
        return false;
      }
    } else if (Existing) {
      const FString ClassPath =
          ReadString(Request.Input->Target, TEXT("actor_class"));
      if (!ClassPath.IsEmpty() &&
          Existing->GetClass()->GetPathName() != ClassPath) {
        SetFailure(OutFailure, TEXT("level_actor_class_conflict"), ClassPath);
        return false;
      }
    }

    const TSharedRef<FLevelAuthoringScratchWorld> ScratchWorld =
        GetScratchWorld();
    AActor *Proposed = Operation == EOperation::Remove
                           ? Existing
                           : ScratchWorld->DuplicateActor(*Template, Error);
    if (Operation != EOperation::Remove && !Proposed) {
      SetFailure(OutFailure, TEXT("level_actor_proposed_create_failed"), Error);
      return false;
    }
    if (Proposed && Operation != EOperation::Remove) {
      ScratchWorld->RegisterPreparedActor(Identity.CanonicalKey, *Proposed);
    }
    TOptional<FString> Label;
    TOptional<FString> Folder;
    TOptional<FTransformPatch> Transform;
    FActorAttachmentPatch Attachment;
    if (!ParseActorExtension(Request.Input->ProviderExtension, Label, Folder,
                             Transform, Error)) {
      SetFailure(OutFailure, TEXT("level_actor_extension_invalid"), Error);
      return false;
    }
    if (!ParseActorAttachmentExtension(Request.Input->ProviderExtension,
                                       Attachment, Error)) {
      SetFailure(OutFailure, TEXT("level_actor_extension_invalid"), Error);
      return false;
    }
    OutState = MakeUnique<FActorLifecycleState>(
        World, ScratchWorld, Operation, MoveTemp(Identity), Existing, Proposed,
        Template->GetActorLabel(), GetActorFolderPathText(*Template),
        Template->GetActorTransform(), MoveTemp(Label), MoveTemp(Folder),
	        MoveTemp(Transform), MoveTemp(Attachment),
	        MoveTemp(PlaceableCandidate));
    return true;
  }

  bool
  PrepareComponent(UWorld &World, const FPrepareRequest &Request,
                   TUniquePtr<IReflectionOperationLifecycleState> &OutState,
                   FOperationFailure &OutFailure) const {
    FString Error;
    UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
    if (!BuildComponentIdentityFromTarget(Request.Input->Target, Identity,
                                          Error)) {
      SetFailure(OutFailure, TEXT("level_component_identity_invalid"), Error);
      return false;
    }
    const TSharedRef<FJsonObject> OwnerTarget =
        MakeOwnerActorTarget(Request.Input->Target);
    UeAgentAuthoringIdentity::FCanonicalIdentity OwnerIdentity;
    if (!BuildActorIdentityFromTarget(OwnerTarget, OwnerIdentity, Error)) {
      SetFailure(OutFailure, TEXT("level_component_owner_invalid"), Error);
      return false;
    }
    AActor *Owner = FindActorExact(World, OwnerTarget, Error);
    if (!Error.IsEmpty()) {
      SetLookupFailure(OutFailure, TEXT("level_component_owner_invalid"), Error,
                       EAuthorabilityPublicTarget::Component);
      return false;
    }
    UActorComponent *Existing = nullptr;
    if (Owner) {
      Existing = FindComponentExact(
          *Owner, Identity.GetSelector(TEXT("component_name")), Error);
      if (!Error.IsEmpty()) {
        SetLookupFailure(OutFailure, TEXT("level_component_identity_invalid"),
                         Error, EAuthorabilityPublicTarget::Component);
        return false;
      }
    }

    const EOperation RequestedOperation = Request.Input->RequestedOperation;
    EOperation Operation = RequestedOperation;
    if (Operation == EOperation::Upsert)
      Operation = Existing ? EOperation::Update : EOperation::Create;
    const bool bResolveUpsertAgainstSpawnedOwner =
        RequestedOperation == EOperation::Upsert && Owner == nullptr;
    if ((Operation == EOperation::Create || Operation == EOperation::Clone) &&
        Existing) {
      SetFailure(OutFailure, TEXT("level_component_already_exists"),
                 Identity.CanonicalKey);
      return false;
    }
    if ((Operation == EOperation::Update || Operation == EOperation::Remove) &&
        !Existing && Owner) {
      SetFailure(OutFailure, TEXT("level_component_not_found"),
                 Identity.CanonicalKey);
      return false;
    }
    if (Operation == EOperation::Remove && !Owner) {
      SetFailure(OutFailure, TEXT("level_component_owner_not_found"),
                 Identity.CanonicalKey);
      return false;
    }

    UActorComponent *Template = Existing;
    if (Operation == EOperation::Create ||
        (!Template && Operation == EOperation::Update)) {
      const FString ClassPath =
          ReadString(Request.Input->Target, TEXT("component_class"));
      UClass *Class =
          StaticLoadClass(UActorComponent::StaticClass(), nullptr, *ClassPath);
      if (Class) {
        const FAuthorabilityResult ClassAuthorability =
            EvaluateComponentClassAuthorability(*Class);
        if (!ClassAuthorability.bAuthorable) {
          SetFailure(OutFailure, ClassAuthorability.ErrorCode,
                     ClassAuthorability.ErrorDetail);
          return false;
        }
      }
      Template = Class ? Class->GetDefaultObject<UActorComponent>() : nullptr;
      if (!Template) {
        SetFailure(OutFailure, TEXT("level_component_class_invalid"),
                   ClassPath);
        return false;
      }
    } else if (Operation == EOperation::Clone) {
      if (!Owner) {
        SetFailure(OutFailure, TEXT("level_component_clone_owner_not_found"));
        return false;
      }
      Template = FindComponentExact(
          *Owner,
          ReadString(Request.Input->Target, TEXT("source_component_name")),
          Error);
      if (!Template) {
        SetLookupFailure(OutFailure,
                         TEXT("level_component_clone_source_not_found"), Error,
                         EAuthorabilityPublicTarget::Component);
        return false;
      }
    } else if (Existing) {
      const FString ClassPath =
          ReadString(Request.Input->Target, TEXT("component_class"));
      if (!ClassPath.IsEmpty() &&
          Existing->GetClass()->GetPathName() != ClassPath) {
        SetFailure(OutFailure, TEXT("level_component_class_conflict"),
                   ClassPath);
        return false;
      }
    }

    const TSharedRef<FLevelAuthoringScratchWorld> ScratchWorld =
        GetScratchWorld();
    UActorComponent *Proposed = Existing;
    if (Operation != EOperation::Remove) {
      AActor *ScratchOwner =
          Owner ? ScratchWorld->DuplicateActor(*Owner, Error)
                : ScratchWorld->FindPreparedActor(OwnerIdentity.CanonicalKey);
      if (!ScratchOwner) {
        SetFailure(OutFailure, TEXT("level_component_scratch_owner_unresolved"),
                   Error.IsEmpty() ? OwnerIdentity.CanonicalKey : Error);
        return false;
      }
      const bool bReuseMatchingComponent =
          Operation == EOperation::Update ||
          RequestedOperation == EOperation::Upsert;
      Proposed = ScratchWorld->CreateComponent(
          *Template, *ScratchOwner,
          FName(*Identity.GetSelector(TEXT("component_name"))),
          bReuseMatchingComponent, Error);
    }
    if (Operation != EOperation::Remove && !Proposed) {
      SetFailure(OutFailure, TEXT("level_component_proposed_create_failed"),
                 Error);
      return false;
    }
    FAttachmentPatch Attachment;
    if (!ParseAttachmentExtension(Request.Input->ProviderExtension, Attachment,
                                  Error)) {
      SetFailure(OutFailure, TEXT("level_component_extension_invalid"), Error);
      return false;
    }
    OutState = MakeUnique<FComponentLifecycleState>(
        World, ScratchWorld, Operation, bResolveUpsertAgainstSpawnedOwner,
        MoveTemp(Identity), OwnerTarget, Existing, Proposed,
        MoveTemp(Attachment));
    return true;
  }

  TWeakObjectPtr<UWorld> BoundWorld;
  TSharedPtr<FLevelAuthoringScratchWorld> SharedScratchWorld;
  bool bHasBoundWorld = false;
};

FInputFieldDescriptor Field(const TCHAR *Name, const TCHAR *Role,
                            const TCHAR *TypeId,
                            const TArray<EOperation> &RequiredFor = {}) {
  FInputFieldDescriptor Result;
  Result.Name = Name;
  Result.Role = Role;
  Result.TypeId = TypeId;
  Result.RequiredFor = RequiredFor;
  return Result;
}

FOperationDescriptor OperationDescriptor(const EOperation Operation) {
  FOperationDescriptor Result;
  Result.Operation = Operation;
  Result.AcceptedSections = {TEXT("target"), TEXT("provider_extension")};
  if (Operation != EOperation::Remove)
    Result.AcceptedSections.Add(TEXT("properties"));
  Result.OmittedFieldPolicy =
      Operation == EOperation::Update   ? TEXT("preserve_current")
      : Operation == EOperation::Remove ? TEXT("not_applicable")
                                        : TEXT("ue_native_default");
  Result.NativeDefaultPolicy =
      Operation == EOperation::Create   ? TEXT("required")
      : Operation == EOperation::Update ? TEXT("forbidden")
      : Operation == EOperation::Remove ? TEXT("not_applicable")
                                        : TEXT("allowed");
  Result.bDestructive = Operation == EOperation::Remove;
  return Result;
}

void ConfigureReflectionContract(FContract &Result) {
  Result.ReflectionBaseline.bEnabled = true;
  Result.ReflectionBaseline.RootKinds = {TEXT("uobject"), TEXT("ustruct")};
  Result.ReflectionBaseline.RecursiveNodes = {
      TEXT("scalar"),    TEXT("struct"),      TEXT("owned_object"),
      TEXT("reference"), TEXT("array"),       TEXT("set"),
      TEXT("map"),       TEXT("fixed_array"), TEXT("optional")};
  Result.ReflectionBaseline.UnknownFieldPolicy = TEXT("reject");
  Result.ReflectionBaseline.ExternalReferencePolicy =
      TEXT("replace_reference_only");
  Result.ReflectionBaseline.OwnedObjectPolicy =
      TEXT("create_or_locate_with_real_outer_then_recurse");
  Result.ReflectionBaseline.RuntimeLimitsSource = TEXT("shared_runtime_limits");
  Result.PropertyPayload.Mode = TEXT("reflection");
  Result.PropertyPayload.PropertyPathField = TEXT("property_path");
  Result.PropertyPayload.ValueField = TEXT("value");
  Result.PropertyPayload.OptionalAssertionFields = {
      TEXT("value_kind"),   TEXT("struct_path"),  TEXT("enum_path"),
      TEXT("element_type"), TEXT("key_type"),     TEXT("mapped_type"),
      TEXT("inner_type"),   TEXT("fixed_length"), TEXT("allowed_class")};
  Result.PropertyPayload.OmittedPatchFieldPolicy = TEXT("preserve_current");
  Result.ProviderId = ApplyProviderId;
  Result.UnknownInputFieldPolicy = TEXT("reject");
  Result.Verification.StateSource = TEXT("shared_state_snapshot");
  Result.Verification.CompareSource = TEXT("shared_semantic_compare");
  Result.Verification.bRequired = true;
}

FContract MakeWorldInputContract() {
  UeAgentApplyInputContract::FContract Result;
  Result.Schema = TEXT("uai.apply_input_contract.v1");
  Result.ContractId = WorldInputContractId;
  Result.ContractVersion = TEXT("1.0.0");
  Result.ProfileId = TEXT("level.world");
  Result.TargetKind = TEXT("level_world");
  Result.SemanticDescriptorRefs = {WorldStateSemanticId};
  Result.InputFields = {
      Field(TEXT("world_path"), TEXT("identity"), TEXT("ue_asset_path"),
            {EOperation::Update}),
      Field(TEXT("operation"), TEXT("operation_control"),
            TEXT("apply_operation"), {EOperation::Update}),
  };
  FOperationDescriptor Update = OperationDescriptor(EOperation::Update);
  Update.IdentityOneOf = {{TEXT("world_path")}};
  Update.AcceptedSections = {TEXT("target")};
  Result.Operations = {Update};
  ConfigureReflectionContract(Result);
  return Result;
}

FContract MakeActorInputContract() {
  UeAgentApplyInputContract::FContract Result;
  Result.Schema = TEXT("uai.apply_input_contract.v1");
  Result.ContractId = ActorInputContractId;
  Result.ContractVersion = TEXT("1.0.0");
  Result.ProfileId = TEXT("level.actor");
  Result.TargetKind = TEXT("level_actor");
  Result.SemanticDescriptorRefs = {ActorStateSemanticId,
                                   ActorPropertySemanticId};
  const TArray<EOperation> All = {EOperation::Create, EOperation::Update,
                                  EOperation::Upsert, EOperation::Remove,
                                  EOperation::Clone};
  Result.InputFields = {
      Field(TEXT("world_path"), TEXT("identity"), TEXT("ue_asset_path"), All),
      Field(TEXT("actor_guid"), TEXT("identity"), TEXT("guid")),
      Field(TEXT("managed_id"), TEXT("identity"), TEXT("string")),
      Field(TEXT("object_path"), TEXT("identity"), TEXT("ue_object_path")),
      Field(TEXT("actor_class"), TEXT("type_assertion"), TEXT("ue_class_path"),
            {EOperation::Create}),
      Field(TEXT("source_actor_guid"), TEXT("selector"), TEXT("guid")),
      Field(TEXT("source_managed_id"), TEXT("selector"), TEXT("string")),
      Field(TEXT("operation"), TEXT("operation_control"),
            TEXT("apply_operation"), All),
      Field(TEXT("properties"), TEXT("intent"),
            TEXT("property_descriptor_list")),
      Field(TEXT("label"), TEXT("provider_extension"), TEXT("string")),
      Field(TEXT("folder_path"), TEXT("provider_extension"), TEXT("string")),
      Field(TEXT("transform"), TEXT("provider_extension"), TEXT("transform")),
      Field(TEXT("attachment"), TEXT("provider_extension"), TEXT("object")),
	  Field(TEXT("candidate"), TEXT("provider_extension"), TEXT("object")),
	  Field(TEXT("placement_context"), TEXT("provider_extension"),
	        TEXT("object")),
  };
  for (const EOperation Item : All) {
    FOperationDescriptor Descriptor = OperationDescriptor(Item);
    Descriptor.IdentityOneOf = {
        {TEXT("world_path"), TEXT("actor_guid")},
        {TEXT("world_path"), TEXT("managed_id")},
        {TEXT("world_path"), TEXT("object_path")},
    };
    Result.Operations.Add(MoveTemp(Descriptor));
  }
  ConfigureReflectionContract(Result);
  return Result;
}

FContract MakeComponentInputContract() {
  UeAgentApplyInputContract::FContract Result;
  Result.Schema = TEXT("uai.apply_input_contract.v1");
  Result.ContractId = ComponentInputContractId;
  Result.ContractVersion = TEXT("1.0.0");
  Result.ProfileId = TEXT("level.component");
  Result.TargetKind = TEXT("level_component");
  Result.SemanticDescriptorRefs = {ComponentStateSemanticId,
                                   ComponentPropertySemanticId};
  const TArray<EOperation> All = {EOperation::Create, EOperation::Update,
                                  EOperation::Upsert, EOperation::Remove,
                                  EOperation::Clone};
  Result.InputFields = {
      Field(TEXT("world_path"), TEXT("identity"), TEXT("ue_asset_path"), All),
      Field(TEXT("owner_actor_guid"), TEXT("identity"), TEXT("guid")),
      Field(TEXT("owner_managed_id"), TEXT("identity"), TEXT("string")),
      Field(TEXT("component_name"), TEXT("identity"), TEXT("name"), All),
      Field(TEXT("component_class"), TEXT("type_assertion"),
            TEXT("ue_class_path"), {EOperation::Create}),
      Field(TEXT("source_component_name"), TEXT("selector"), TEXT("name"),
            {EOperation::Clone}),
      Field(TEXT("operation"), TEXT("operation_control"),
            TEXT("apply_operation"), All),
      Field(TEXT("properties"), TEXT("intent"),
            TEXT("property_descriptor_list")),
      Field(TEXT("attachment"), TEXT("provider_extension"), TEXT("object")),
  };
  for (const EOperation Item : All) {
    FOperationDescriptor Descriptor = OperationDescriptor(Item);
    Descriptor.IdentityOneOf = {
        {TEXT("world_path"), TEXT("owner_actor_guid"), TEXT("component_name")},
        {TEXT("world_path"), TEXT("owner_managed_id"), TEXT("component_name")},
    };
    Result.Operations.Add(MoveTemp(Descriptor));
  }
  ConfigureReflectionContract(Result);
  return Result;
}

UeAgentApplyWorkspaceContract::FContract MakeWorkspaceContract() {
  using namespace UeAgentApplyWorkspaceContract;
  UeAgentApplyWorkspaceContract::FContract Result;
  Result.Schema = TEXT("uai.apply_workspace_contract.v1");
  Result.ContractId = WorkspaceContractId;
  Result.ContractVersion = TEXT("1.0.0");
  Result.ProfileId = TEXT("level.authoring");
  Result.Sources = {
      {ESourceKind::Directory, TEXT("physical"), TEXT("index")},
      {ESourceKind::SingleFile, TEXT("virtual_bundle"), TEXT("index")},
      {ESourceKind::Inline, TEXT("virtual_bundle"), TEXT("index")},
  };
  Result.Entry.DirectoryEntry = TEXT("index.hjson");
  Result.Entry.VirtualUnitId = TEXT("world");
  FUnitDescriptor World;
  World.UnitId = TEXT("world");
  World.PathPattern = TEXT("index.hjson");
  World.Role = TEXT("index");
  World.Cardinality = TEXT("one");
  World.bRequired = true;
  World.RouteId = TEXT("level.world");
  World.ApplyContractRef = WorldInputContractId;
  World.MergePolicy = TEXT("replace");
  World.AbsentSemantics = TEXT("error");
  World.EmptySemantics = TEXT("error");
  FUnitDescriptor Actor;
  Actor.UnitId = TEXT("actor");
  Actor.PathPattern = TEXT("actors/items/*/actor.hjson");
  Actor.Role = TEXT("object");
  Actor.Cardinality = TEXT("zero_or_more");
  Actor.RouteId = TEXT("level.actor");
  Actor.ApplyContractRef = ActorInputContractId;
  Actor.MergePolicy = TEXT("replace");
  Actor.AbsentSemantics = TEXT("no_change");
  Actor.EmptySemantics = TEXT("error");
  FUnitDescriptor Component;
  Component.UnitId = TEXT("component");
  Component.PathPattern = TEXT("actors/items/*/components/*/component.hjson");
  Component.Role = TEXT("object");
  Component.Cardinality = TEXT("zero_or_more");
  Component.RouteId = TEXT("level.component");
  Component.ApplyContractRef = ComponentInputContractId;
  Component.MergePolicy = TEXT("replace");
  Component.AbsentSemantics = TEXT("no_change");
  Component.EmptySemantics = TEXT("error");
  Result.Units = {World, Actor, Component};
  Result.ReferencePolicy.Base = TEXT("workspace_root");
  Result.ReferencePolicy.RootContainment = TEXT("required");
  Result.ReferencePolicy.ExternalReferences = TEXT("reject");
  Result.ReferencePolicy.CaseCollision = TEXT("reject");
  Result.DependencyPolicy.EdgeSource = TEXT("contract_and_references");
  Result.DependencyPolicy.CyclePolicy = TEXT("reject");
  Result.DependencyPolicy.UnresolvedPolicy = TEXT("reject");
  Result.DependencyPolicy.Order = TEXT("stable_topological");
  Result.Preflight.Scope = TEXT("whole_workspace");
  Result.Preflight.MutationBeforeSuccess = TEXT("forbid");
  Result.Preflight.bDiagnosticsIncludeSource = true;
  Result.Transaction.Mode = TEXT("atomic");
  Result.Transaction.RollbackOn = {TEXT("mutation_failed"),
                                   TEXT("compile_failed"), TEXT("save_failed"),
                                   TEXT("verification_failed")};
  Result.Execution.Compile = TEXT("provider_hook");
  Result.Execution.Save = TEXT("caller_controlled");
  Result.Execution.Verification = TEXT("shared_snapshot_required");
  Result.UnknownFilePolicy = TEXT("reject");
  Result.DuplicateTargetPolicy = TEXT("reject");
  return Result;
}

bool GetOperation(const UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit &Unit,
                  EOperation &OutOperation) {
  FString Text;
  return Unit.Document.IsValid() &&
         Unit.Document->TryGetStringField(TEXT("operation"), Text) &&
         ParseOperation(Text, OutOperation);
}

bool GetTarget(const UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit &Unit,
               TSharedPtr<const FJsonObject> &OutTarget) {
  OutTarget.Reset();
  const TSharedPtr<FJsonObject> *Target = nullptr;
  if (!Unit.Document.IsValid() ||
      !Unit.Document->TryGetObjectField(TEXT("target"), Target) || !Target ||
      !Target->IsValid()) {
    return false;
  }
  OutTarget = *Target;
  return true;
}

bool BuildDependenciesAndValidate(
    UWorld &World, const UeAgentAuthoringWorkspaceRouter::FWorkspace &Workspace,
    UeAgentAuthoringWorkspaceDependencyGraph::FDependencyInputs &OutInputs,
    FString &OutErrorCode, FString &OutError) {
  OutInputs = {};
  OutErrorCode.Reset();
  OutError.Reset();
  TMap<FString, const UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit *>
      ActorUnits;
  TMap<FString, EOperation> ActorOperations;
  TMap<FString, const UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit *>
      ComponentUnits;
  for (const UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit &Unit :
       Workspace.Units) {
    if (Unit.UnitId == TEXT("world") &&
        Unit.Action == UeAgentAuthoringWorkspaceRouter::EUnitAction::Apply) {
      TSharedPtr<const FJsonObject> Target;
      if (!GetTarget(Unit, Target) ||
          ReadString(Target, TEXT("world_path")) != GetWorldPath(World)) {
        OutError = TEXT("level_workspace_world_conflict");
        return false;
      }
    } else if (Unit.UnitId == TEXT("actor") &&
               Unit.Action ==
                   UeAgentAuthoringWorkspaceRouter::EUnitAction::Apply) {
      TSharedPtr<const FJsonObject> Target;
      UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
      EOperation Operation;
      FString Error;
      if (!GetTarget(Unit, Target) ||
          !BuildActorIdentityFromTarget(Target, Identity, Error) ||
          !GetOperation(Unit, Operation)) {
        OutError = Error.IsEmpty()
                       ? TEXT("level_actor_unit_invalid:") + Unit.RelativePath
                       : Error;
        return false;
      }
      ActorUnits.Add(Identity.CanonicalKey, &Unit);
      ActorOperations.Add(Identity.CanonicalKey, Operation);
      UeAgentAuthoringWorkspaceDependencyGraph::FDependencyDeclaration
          Dependency;
      Dependency.SourceUnitKey = Unit.CanonicalUnitKey;
      Dependency.TargetReference = TEXT("world");
      Dependency.SourceJsonPointer = TEXT("/target/world_path");
      OutInputs.ContractDependencies.Add(MoveTemp(Dependency));
    } else if (Unit.UnitId == TEXT("component") &&
               Unit.Action ==
                   UeAgentAuthoringWorkspaceRouter::EUnitAction::Apply) {
      TSharedPtr<const FJsonObject> Target;
      UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
      FString Error;
      if (!GetTarget(Unit, Target) ||
          !BuildComponentIdentityFromTarget(Target, Identity, Error)) {
        OutError = Error.IsEmpty() ? TEXT("level_component_unit_invalid:") +
                                         Unit.RelativePath
                                   : Error;
        return false;
      }
      ComponentUnits.Add(Identity.CanonicalKey, &Unit);
    }
  }

  for (const UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit &Unit :
       Workspace.Units) {
    if (Unit.UnitId != TEXT("actor") ||
        Unit.Action != UeAgentAuthoringWorkspaceRouter::EUnitAction::Apply)
      continue;
    const TSharedPtr<const FJsonObject> Extension =
        ReadObject(Unit.Document, TEXT("provider_extension"));
    const TSharedPtr<const FJsonObject> Attachment =
        ReadObject(Extension, TEXT("attachment"));
    const FString ParentManagedId =
        ReadString(Attachment, TEXT("parent_managed_id"));
    if (ParentManagedId.IsEmpty())
      continue;
    TSharedPtr<const FJsonObject> Target;
    if (!GetTarget(Unit, Target)) {
      OutError = TEXT("level_actor_unit_invalid:") + Unit.RelativePath;
      return false;
    }
    TSharedRef<FJsonObject> ParentTarget = MakeShared<FJsonObject>();
    ParentTarget->SetStringField(TEXT("world_path"),
                                 ReadString(Target, TEXT("world_path")));
    ParentTarget->SetStringField(TEXT("managed_id"), ParentManagedId);
    UeAgentAuthoringIdentity::FCanonicalIdentity ParentIdentity;
    FString ParentError;
    if (!BuildActorIdentityFromTarget(ParentTarget, ParentIdentity,
                                      ParentError)) {
      OutError = ParentError;
      return false;
    }
    const UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit *const *ParentUnit =
        ActorUnits.Find(ParentIdentity.CanonicalKey);
    AActor *ExistingParent = FindActorExact(World, ParentTarget, ParentError);
    if (!ParentUnit && !ExistingParent) {
      OutErrorCode = TEXT("level_actor_attachment_parent_not_found");
      OutError = ParentError.IsEmpty() ? ParentManagedId : ParentError;
      return false;
    }
    if (ParentUnit) {
      if (ActorOperations.FindRef(ParentIdentity.CanonicalKey) ==
          EOperation::Remove) {
        OutErrorCode = TEXT("level_actor_attachment_parent_removed");
        OutError = ParentManagedId;
        return false;
      }
      UeAgentAuthoringWorkspaceDependencyGraph::FDependencyDeclaration
          Dependency;
      Dependency.SourceUnitKey = Unit.CanonicalUnitKey;
      Dependency.TargetReference = (*ParentUnit)->RelativePath;
      Dependency.SourceJsonPointer =
          TEXT("/provider_extension/attachment/parent_managed_id");
      OutInputs.ReferenceDependencies.Add(MoveTemp(Dependency));
    }
  }

  for (const UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit &Unit :
       Workspace.Units) {
    if (Unit.UnitId != TEXT("component") ||
        Unit.Action != UeAgentAuthoringWorkspaceRouter::EUnitAction::Apply)
      continue;
    TSharedPtr<const FJsonObject> Target;
    EOperation ComponentOperation;
    if (!GetTarget(Unit, Target) || !GetOperation(Unit, ComponentOperation)) {
      OutError = TEXT("level_component_unit_invalid:") + Unit.RelativePath;
      return false;
    }
    const TSharedRef<FJsonObject> OwnerTarget = MakeOwnerActorTarget(Target);
    UeAgentAuthoringIdentity::FCanonicalIdentity OwnerIdentity;
    FString Error;
    if (!BuildActorIdentityFromTarget(OwnerTarget, OwnerIdentity, Error)) {
      OutError = Error;
      return false;
    }
    const UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit *const *OwnerUnit =
        ActorUnits.Find(OwnerIdentity.CanonicalKey);
    AActor *ExistingOwner = FindActorExact(World, OwnerTarget, Error);
    if (!OwnerUnit && !ExistingOwner) {
      FAuthorabilityResult Authorability;
      if (TryNormalizeAuthorabilityFailure(
              Error, EAuthorabilityPublicTarget::Component, Authorability)) {
        OutErrorCode = Authorability.ErrorCode;
        OutError = Authorability.ErrorDetail;
      } else {
        OutError = Error.IsEmpty() ? TEXT("level_component_owner_unresolved:") +
                                         Unit.RelativePath
                                   : Error;
      }
      return false;
    }
    if (OwnerUnit) {
      const EOperation OwnerOperation =
          ActorOperations.FindRef(OwnerIdentity.CanonicalKey);
      if (OwnerOperation == EOperation::Remove &&
          ComponentOperation != EOperation::Remove) {
        OutError = TEXT("level_component_owner_removed:") + Unit.RelativePath;
        return false;
      }
      UeAgentAuthoringWorkspaceDependencyGraph::FDependencyDeclaration
          Dependency;
      Dependency.SourceUnitKey = Unit.CanonicalUnitKey;
      Dependency.TargetReference = (*OwnerUnit)->RelativePath;
      Dependency.SourceJsonPointer = TEXT("/target");
      Dependency.Order = OwnerOperation == EOperation::Remove
                             ? UeAgentAuthoringWorkspaceDependencyGraph::
                                   EOrderConstraint::SourceBeforeTarget
                             : UeAgentAuthoringWorkspaceDependencyGraph::
                                   EOrderConstraint::TargetBeforeSource;
      OutInputs.ReferenceDependencies.Add(MoveTemp(Dependency));
    }

    const TSharedPtr<const FJsonObject> Extension =
        ReadObject(Unit.Document, TEXT("provider_extension"));
    const TSharedPtr<const FJsonObject> Attachment =
        ReadObject(Extension, TEXT("attachment"));
    const FString ParentName = ReadString(Attachment, TEXT("parent_component"));
    if (ParentName.IsEmpty())
      continue;
    UeAgentAuthoringIdentity::FCanonicalIdentity ParentIdentity;
    if (!UeAgentAuthoringIdentity::BuildCanonicalIdentity(
            ComponentIdentityModelId, TEXT("level_component"),
            {
                {TEXT("world_path"), ReadString(Target, TEXT("world_path"))},
                {TEXT("owner_actor_guid"),
                 ReadString(Target, TEXT("owner_actor_guid"))},
                {TEXT("owner_managed_id"),
                 ReadString(Target, TEXT("owner_managed_id"))},
                {TEXT("component_name"), ParentName},
            },
            ParentIdentity, Error)) {
      OutError = Error;
      return false;
    }
    const UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit *const *ParentUnit =
        ComponentUnits.Find(ParentIdentity.CanonicalKey);
    FString ParentError;
    UActorComponent *ExistingParent =
        ExistingOwner
            ? FindComponentExact(*ExistingOwner, ParentName, ParentError)
            : nullptr;
    bool bNativeDefaultParent = false;
    if (!ParentUnit && !ExistingParent && OwnerUnit) {
      TSharedPtr<const FJsonObject> OwnerUnitTarget;
      const FString OwnerClassPath =
          GetTarget(**OwnerUnit, OwnerUnitTarget)
              ? ReadString(OwnerUnitTarget, TEXT("actor_class"))
              : FString();
      UClass *OwnerClass = OwnerClassPath.IsEmpty()
                               ? nullptr
                               : StaticLoadClass(AActor::StaticClass(), nullptr,
                                                 *OwnerClassPath);
      AActor *OwnerDefault =
          OwnerClass ? OwnerClass->GetDefaultObject<AActor>() : nullptr;
      if (OwnerDefault) {
        TArray<UObject *> DefaultSubobjects;
        OwnerDefault->GetDefaultSubobjects(DefaultSubobjects);
        bNativeDefaultParent = DefaultSubobjects.ContainsByPredicate(
            [&ParentName](const UObject *Candidate) {
              const UActorComponent *Component =
                  Cast<UActorComponent>(Candidate);
              return Component && Component->GetName().Equals(
                                      ParentName,
                                      ESearchCase::CaseSensitive);
            });
      }
    }
    if (!ParentUnit && !ExistingParent && !bNativeDefaultParent) {
      FAuthorabilityResult Authorability;
      if (TryNormalizeAuthorabilityFailure(
              ParentError, EAuthorabilityPublicTarget::Component,
              Authorability)) {
        OutErrorCode = Authorability.ErrorCode;
        OutError = Authorability.ErrorDetail;
      } else {
        OutError = ParentError.IsEmpty()
                       ? TEXT("level_component_parent_unresolved:") + ParentName
                       : ParentError;
      }
      return false;
    }
    if (ParentUnit) {
      UeAgentAuthoringWorkspaceDependencyGraph::FDependencyDeclaration
          Dependency;
      Dependency.SourceUnitKey = Unit.CanonicalUnitKey;
      Dependency.TargetReference = (*ParentUnit)->RelativePath;
      Dependency.SourceJsonPointer =
          TEXT("/provider_extension/attachment/parent_component");
      OutInputs.ReferenceDependencies.Add(MoveTemp(Dependency));
    }
  }
  return true;
}

void AddMetadataCounters(const TMap<FString, TSharedPtr<FJsonObject>> &Metadata,
                         FWorkspaceApplyResult &OutResult) {
  auto Add = [&Metadata](const TCHAR *Field, int32 &OutValue) {
    for (const TPair<FString, TSharedPtr<FJsonObject>> &Pair : Metadata) {
      double Value = 0.0;
      if (Pair.Value.IsValid() && Pair.Value->TryGetNumberField(Field, Value)) {
        OutValue += FMath::RoundToInt(Value);
      }
    }
  };
  Add(TEXT("actors_created"), OutResult.ActorsCreated);
  Add(TEXT("actors_updated"), OutResult.ActorsUpdated);
  Add(TEXT("actors_removed"), OutResult.ActorsRemoved);
  Add(TEXT("components_created"), OutResult.ComponentsCreated);
  Add(TEXT("components_updated"), OutResult.ComponentsUpdated);
  Add(TEXT("components_removed"), OutResult.ComponentsRemoved);
}

FString CanonicalDocument(const TSharedPtr<const FJsonObject> &Document) {
  return Document.IsValid()
             ? UeAgentAuthoringState::MakeStableCanonicalValueKey(
                   MakeShared<FJsonValueObject>(
                       ConstCastSharedPtr<FJsonObject>(Document)))
             : FString();
}

TSharedRef<IApplyProvider> MakeBoundApplyProvider(UWorld &World) {
  const TSharedRef<IReflectionOperationLifecycle> Lifecycle =
      MakeShared<FLevelLifecycle>(World);
  UeAgentAuthoringObjectApply::FOptions Options;
  Options.bAllowDomainAuthorizedOwners = true;
  Options.bAllowTemplateOwners = true;
  return MakeShared<FReflectionApplyProvider>(ApplyProviderId, Lifecycle,
                                              Options);
}
} // namespace

TSharedRef<FJsonObject> FWorkspaceApplyResult::ToJsonObject() const {
  TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
  Result->SetStringField(TEXT("status"), Status);
  Result->SetStringField(TEXT("phase"), Phase);
  if (!ErrorCode.IsEmpty())
    Result->SetStringField(TEXT("error_code"), ErrorCode);
  if (!ErrorDetail.IsEmpty())
    Result->SetStringField(TEXT("error_detail"), ErrorDetail);
  Result->SetNumberField(TEXT("units_checked"), UnitsChecked);
  Result->SetNumberField(TEXT("mutations_applied"), MutationsApplied);
  Result->SetNumberField(TEXT("actors_created"), ActorsCreated);
  Result->SetNumberField(TEXT("actors_updated"), ActorsUpdated);
  Result->SetNumberField(TEXT("actors_removed"), ActorsRemoved);
  Result->SetNumberField(TEXT("components_created"), ComponentsCreated);
  Result->SetNumberField(TEXT("components_updated"), ComponentsUpdated);
  Result->SetNumberField(TEXT("components_removed"), ComponentsRemoved);
  Result->SetBoolField(TEXT("rollback_attempted"), bRollbackAttempted);
  Result->SetBoolField(TEXT("rollback_succeeded"), bRollbackSucceeded);
  return Result;
}

bool RegisterApplyContracts(FString &OutError) {
  UeAgentApplyInputContract::ERegistrationResult InputResult;
  if (!UeAgentApplyInputContract::FRegistry::Get().RegisterContract(
          MakeWorldInputContract(), InputResult, OutError) ||
      !UeAgentApplyInputContract::FRegistry::Get().RegisterContract(
          MakeActorInputContract(), InputResult, OutError) ||
      !UeAgentApplyInputContract::FRegistry::Get().RegisterContract(
          MakeComponentInputContract(), InputResult, OutError)) {
    return false;
  }
  UeAgentApplyWorkspaceContract::ERegistrationResult WorkspaceResult;
  return UeAgentApplyWorkspaceContract::FRegistry::Get().RegisterContract(
      MakeWorkspaceContract(), WorkspaceResult, OutError);
}

TSharedRef<IApplyProvider> MakeApplyProvider() {
  static const TSharedRef<IReflectionOperationLifecycle> Lifecycle =
      MakeShared<FLevelLifecycle>();
  UeAgentAuthoringObjectApply::FOptions Options;
  Options.bAllowDomainAuthorizedOwners = true;
  Options.bAllowTemplateOwners = true;
  return MakeShared<FReflectionApplyProvider>(ApplyProviderId, Lifecycle,
                                              Options);
}

bool ApplyWorkspaceSource(
    UWorld &World,
    const UeAgentAuthoringWorkspaceLoader::FSourceRequest &Source,
    const FWorkspaceApplyOptions &Options, FWorkspaceApplyResult &OutResult,
    const FString &RequestId) {
  OutResult = FWorkspaceApplyResult();
  OutResult.Status = TEXT("failed");
  UeAgentApplyWorkspaceContract::FContract WorkspaceContract;
  if (!UeAgentApplyWorkspaceContract::FRegistry::Get().FindContract(
          WorkspaceContractId, WorkspaceContract)) {
    OutResult.Phase = TEXT("registration");
    OutResult.ErrorCode = TEXT("level_workspace_contract_missing");
    return false;
  }

  UeAgentAuthoringWorkspaceLoader::FLoadResult LoadResult;
  if (!UeAgentAuthoringWorkspaceLoader::LoadWorkspace(WorkspaceContract, Source,
                                                      LoadResult)) {
    OutResult.Phase = TEXT("load");
    OutResult.ErrorCode = LoadResult.ErrorCode;
    OutResult.ErrorDetail = LoadResult.ErrorDetail;
    return false;
  }

  UeAgentAuthoringWorkspaceDependencyGraph::FDependencyInputs DependencyInputs;
  FString DependencyErrorCode;
  if (!BuildDependenciesAndValidate(World, LoadResult.Workspace,
                                    DependencyInputs, DependencyErrorCode,
                                    OutResult.ErrorDetail)) {
    OutResult.Phase = TEXT("preflight");
    OutResult.ErrorCode = DependencyErrorCode.IsEmpty()
                              ? TEXT("level_workspace_reference_invalid")
                              : DependencyErrorCode;
    return false;
  }
  UeAgentAuthoringWorkspaceDependencyGraph::FDependencyGraph Graph;
  UeAgentAuthoringWorkspaceDependencyGraph::FGraphFailure GraphFailure;
  if (!UeAgentAuthoringWorkspaceDependencyGraph::BuildDependencyGraph(
          WorkspaceContract, LoadResult.Workspace, DependencyInputs, Graph,
          GraphFailure)) {
    OutResult.Phase = TEXT("preflight");
    OutResult.ErrorCode = GraphFailure.ErrorCode;
    OutResult.ErrorDetail = GraphFailure.ErrorDetail;
    return false;
  }

  UeAgentAuthoringOperationApply::FProviderRegistry ScopedProviders;
  UeAgentAuthoringOperationApply::ERegistrationResult ProviderRegistration;
  FString ProviderRegistrationError;
  if (!ScopedProviders.RegisterProvider(MakeBoundApplyProvider(World),
                                        ProviderRegistration,
                                        ProviderRegistrationError)) {
    OutResult.Phase = TEXT("registration");
    OutResult.ErrorCode =
        TEXT("level_bound_apply_provider_registration_failed");
    OutResult.ErrorDetail = ProviderRegistrationError;
    return false;
  }

  if (Options.bDryRun) {
    UeAgentAuthoringOperationApply::FWorkspaceOperationPreflightProvider
        Provider(UeAgentApplyInputContract::FRegistry::Get(), ScopedProviders);
    UeAgentAuthoringWorkspaceTransaction::FPreflightResult Preflight;
    if (!UeAgentAuthoringWorkspaceTransaction::PreflightWorkspace(
            WorkspaceContract, LoadResult.Workspace, Graph, Provider,
            Preflight)) {
      OutResult.Phase = TEXT("preflight");
      OutResult.ErrorCode = Preflight.CauseCode.IsEmpty() ? Preflight.ErrorCode
                                                          : Preflight.CauseCode;
      OutResult.ErrorDetail = Preflight.ErrorDetail;
      OutResult.UnitsChecked = Preflight.UnitsChecked;
      return false;
    }
    OutResult.Status = TEXT("ready");
    OutResult.Phase = TEXT("preflight");
    OutResult.UnitsChecked = Preflight.UnitsChecked;
    return true;
  }

  UeAgentAuthoringWorkspaceTransaction::FExecutionOptions ExecutionOptions;
  UeAgentAuthoringOperationApply::FOperationFailure RequestFailure;
  if (!UeAgentAuthoringOperationApply::BuildApplyEntryExecutionOptions(
          RequestId, ExecutionOptions, RequestFailure)) {
    OutResult.Phase = TEXT("preflight");
    OutResult.ErrorCode = RequestFailure.ErrorCode;
    OutResult.ErrorDetail = RequestFailure.ErrorDetail;
    return false;
  }
  UeAgentAuthoringOperationApply::FBackendOptions BackendOptions;
  BackendOptions.bUseEditorTransaction = Options.bUseEditorTransaction;
  BackendOptions.TransactionDescription =
      NSLOCTEXT("UeAgentInterface", "LevelAuthoringWorkspaceApply",
                "Apply Level authoring workspace");
  UeAgentAuthoringOperationApply::FApplyWorkspaceResult ApplyResult;
  const bool bApplied = UeAgentAuthoringOperationApply::ApplyWorkspace(
      WorkspaceContract, LoadResult.Workspace, Graph,
      UeAgentApplyInputContract::FRegistry::Get(), ScopedProviders,
      ExecutionOptions, BackendOptions, ApplyResult);
  OutResult.UnitsChecked = ApplyResult.Preflight.UnitsChecked;
  OutResult.MutationsApplied = ApplyResult.Execution.MutationsApplied;
  OutResult.bRollbackAttempted = ApplyResult.Execution.bRollbackAttempted;
  OutResult.bRollbackSucceeded = ApplyResult.Execution.bRollbackSucceeded;
  if (!bApplied) {
    const bool bPreflight = !ApplyResult.Preflight.IsReady();
    OutResult.Phase =
        bPreflight ? TEXT("preflight") : ApplyResult.Execution.Phase;
    OutResult.ErrorCode = bPreflight ? ApplyResult.Preflight.CauseCode
                                     : ApplyResult.Execution.CauseCode;
    if (OutResult.ErrorCode.IsEmpty()) {
      OutResult.ErrorCode = bPreflight ? ApplyResult.Preflight.ErrorCode
                                       : ApplyResult.Execution.ErrorCode;
    }
    OutResult.ErrorDetail = bPreflight ? ApplyResult.Preflight.ErrorDetail
                                       : ApplyResult.Execution.ErrorDetail;
    return false;
  }
  OutResult.Status = TEXT("applied");
  OutResult.Phase = TEXT("complete");
  AddMetadataCounters(ApplyResult.UnitResultMetadata, OutResult);
  return true;
}

bool ApplyWorkspaceDirectory(UWorld &World, const FString &WorkspaceRoot,
                             const FWorkspaceApplyOptions &Options,
                             FWorkspaceApplyResult &OutResult,
                             const FString &RequestId) {
  return ApplyWorkspaceSource(
      World,
      UeAgentAuthoringWorkspaceLoader::FSourceRequest::MakeDirectory(
          WorkspaceRoot),
      Options, OutResult, RequestId);
}

bool ValidateWorkspaceDirectory(UWorld &World, const FString &WorkspaceRoot,
                                FWorkspaceApplyResult &OutResult) {
  FWorkspaceApplyOptions Options;
  Options.bDryRun = true;
  Options.bUseEditorTransaction = false;
  return ApplyWorkspaceDirectory(World, WorkspaceRoot, Options, OutResult);
}

bool DiffWorkspaceDirectories(const FString &BaseRoot,
                              const FString &TargetRoot, const int32 MaxChanges,
                              TSharedPtr<FJsonObject> &OutSummary,
                              FString &OutError) {
  OutSummary = MakeShared<FJsonObject>();
  OutError.Reset();
  UeAgentApplyWorkspaceContract::FContract Contract;
  if (!UeAgentApplyWorkspaceContract::FRegistry::Get().FindContract(
          WorkspaceContractId, Contract)) {
    OutError = TEXT("level_workspace_contract_missing");
    return false;
  }
  UeAgentAuthoringWorkspaceLoader::FLoadResult Base;
  UeAgentAuthoringWorkspaceLoader::FLoadResult Target;
  if (!UeAgentAuthoringWorkspaceLoader::LoadWorkspace(
          Contract,
          UeAgentAuthoringWorkspaceLoader::FSourceRequest::MakeDirectory(
              BaseRoot),
          Base) ||
      !UeAgentAuthoringWorkspaceLoader::LoadWorkspace(
          Contract,
          UeAgentAuthoringWorkspaceLoader::FSourceRequest::MakeDirectory(
              TargetRoot),
          Target)) {
    OutError = !Base.IsLoaded()
                   ? Base.ErrorCode + TEXT(":") + Base.ErrorDetail
                   : Target.ErrorCode + TEXT(":") + Target.ErrorDetail;
    return false;
  }
  TMap<FString, FString> BaseDocuments;
  TMap<FString, FString> TargetDocuments;
  for (const UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit &Unit :
       Base.Workspace.Units) {
    if (Unit.Action == UeAgentAuthoringWorkspaceRouter::EUnitAction::Apply) {
      BaseDocuments.Add(Unit.RelativePath.ToLower(),
                        CanonicalDocument(Unit.Document));
    }
  }
  for (const UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit &Unit :
       Target.Workspace.Units) {
    if (Unit.Action == UeAgentAuthoringWorkspaceRouter::EUnitAction::Apply) {
      TargetDocuments.Add(Unit.RelativePath.ToLower(),
                          CanonicalDocument(Unit.Document));
    }
  }
  TSet<FString> Paths;
  for (const TPair<FString, FString> &Pair : BaseDocuments)
    Paths.Add(Pair.Key);
  for (const TPair<FString, FString> &Pair : TargetDocuments)
    Paths.Add(Pair.Key);
  TArray<FString> SortedPaths = Paths.Array();
  SortedPaths.Sort();
  TArray<TSharedPtr<FJsonValue>> Changes;
  const int32 Limit = MaxChanges > 0 ? MaxChanges : 256;
  for (const FString &Path : SortedPaths) {
    const FString *Left = BaseDocuments.Find(Path);
    const FString *Right = TargetDocuments.Find(Path);
    if (Left && Right && *Left == *Right)
      continue;
    if (Changes.Num() >= Limit)
      break;
    TSharedRef<FJsonObject> Change = MakeShared<FJsonObject>();
    Change->SetStringField(TEXT("path"), Path);
    Change->SetStringField(TEXT("kind"), !Left    ? TEXT("added")
                                         : !Right ? TEXT("removed")
                                                  : TEXT("changed"));
    Changes.Add(MakeShared<FJsonValueObject>(Change));
  }
  OutSummary->SetStringField(TEXT("schema"),
                             TEXT("uai.level.workspace_diff.v1"));
  OutSummary->SetBoolField(TEXT("equal"), Changes.IsEmpty());
  OutSummary->SetNumberField(TEXT("change_count"), Changes.Num());
  OutSummary->SetBoolField(TEXT("truncated"), Changes.Num() >= Limit);
  OutSummary->SetArrayField(TEXT("changes"), Changes);
  return true;
}
} // namespace UeAgentLevelAuthoring
