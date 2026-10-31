// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Level/Authoring/UeAgentLevelAuthoringApplyProvider.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/AssetDurable/UeAgentPersistedWorld.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"

#include "AI/NavigationSystemBase.h"
#include "ComponentAssetBroker.h"
#include "Components/ActorComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/RectLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "Core/ContentDiscovery/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "Core/ContentDiscovery/PaletteSearch/UeAgentBrushBuilderActorFactory.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringAuthorability.h"
#include "Domains/Level/Authoring/UeAgentLevelActorDeletion.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringDescriptors.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringExport.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringScratchWorld.h"
#include "WorldPartition/HLOD/HLODActor.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringSpatial.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringState.h"
#include "Domains/Level/Authoring/UeAgentLevelWorldTopology.h"
#include "Domains/PCG/Authoring/UeAgentPcgComponentAuthoring.h"
#include "Engine/Engine.h"
#include "Engine/Brush.h"
#include "Engine/Level.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Editor.h"
#include "EditorLevelUtils.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "HAL/PlatformTime.h"
#include "NavigationData.h"
#include "NavigationSystem.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/NoExportTypes.h"
#include "UObject/StrongObjectPtr.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
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





using UE::UAI::Contract::FDiagnostic;
using UeAgentAuthoringDiagnostics::FScopedSourceLocations;

FDiagnostic LevelFailure(const TCHAR* Code, const FString& Reason,
    const FString& Field, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
    const FJsonObject* Input = nullptr, const FString& InputField = FString(),
    const FString& Pointer = FString()) {
  auto Error = FDiagnostic::Failure(Code, Reason, TEXT("level_apply"));
  Error.PropertyPath = Field;
  Error.ActualValuePresent = Actual.IsValid();
  Error.ActualValue = MoveTemp(Actual);
  Error.ExpectedValue = MoveTemp(Expected);
  Error.Source.JsonPath = Pointer;
  if (Input) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Input, InputField);
  else Error.SourceKind = TEXT("native_result");
  return Error;
}

FDiagnostic PresenceFailure(const TCHAR* Code, const FString& Reason, const FString& Field,
    bool bActual, bool bExpected, const FString& TargetPath = FString(), const FString& Candidate = FString()) {
  auto Error = LevelFailure(Code, Reason, Field, MakeShared<FJsonValueBoolean>(bActual), MakeShared<FJsonValueBoolean>(bExpected));
  Error.TargetPath = TargetPath;
  Error.CandidateName = Candidate;
  return Error;
}

FDiagnostic TargetPresenceFailure(const TCHAR* Code, const FString& Reason,
    const TSharedPtr<const FJsonObject>& Target, const FString& Field, bool bActual, bool bExpected) {
  auto Error = LevelFailure(Code, Reason, Field, MakeShared<FJsonValueBoolean>(bActual),
      MakeShared<FJsonValueBoolean>(bExpected), Target.Get());
  if (Target.IsValid()) {
    Target->TryGetStringField(TEXT("world_path"), Error.TargetPath);
    Target->TryGetStringField(TEXT("component_name"), Error.CandidateName);
  }
  return Error;
}

class FDerivedTargetSources {
public:
  FDerivedTargetSources(const FJsonObject* Derived, const FJsonObject* Input, TMap<FString, FString> Fields)
      : Scope(Derived, [Input, Fields = MoveTemp(Fields)](const FString& Pointer) {
          UE::UAI::Contract::FSourceLocation Source;
          if (!Input) return Source;
          if (const FString* Field = Fields.Find(Pointer)) FScopedSourceLocations::Locate(*Input, *Field, Source);
          else if (Pointer.IsEmpty() || Pointer == TEXT("/")) FScopedSourceLocations::Locate(*Input, FString(), Source);
          return Source;
        }) {}
private:
  FScopedSourceLocations Scope;
};

void SetFailure(FOperationFailure& OutFailure, UE::UAI::Contract::FDiagnostic Diagnostic) { OutFailure.Diagnostic = MoveTemp(Diagnostic); }

FDiagnostic PublicLookupFailure(const FDiagnostic& LookupError, const EAuthorabilityPublicTarget PublicTarget) {
  FAuthorabilityResult Authorability;
  return TryMapAuthorabilityFailure(LookupError, PublicTarget, Authorability)
      ? Authorability.Diagnostic : LookupError;
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
    UE::UAI::Contract::FDiagnostic& OutError) {
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
    UE::UAI::Contract::FDiagnostic& OutError) {
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
                         TOptional<TArray<FName>> &OutTags,
                         UE::UAI::Contract::FDiagnostic& OutError) {
  OutLabel.Reset();
  OutFolder.Reset();
  OutTransform.Reset();
  OutTags.Reset();
  OutError.Reset();
  if (!Extension.IsValid())
    return true;
  if (Extension->HasField(TEXT("label"))) {
    FString Value;
    if (!Extension->TryGetStringField(TEXT("label"), Value)) {
      OutError = LevelFailure(TEXT("level_actor_label_invalid"), TEXT("Actor label must be a string."),
          TEXT("label"), Extension->TryGetField(TEXT("label")), MakeShared<FJsonValueString>(TEXT("string")),
          Extension.Get(), TEXT("label"));
      return false;
    }
    OutLabel = Value;
  }
  if (Extension->HasField(TEXT("folder_path"))) {
    FString Value;
    if (!Extension->TryGetStringField(TEXT("folder_path"), Value)) {
      OutError = LevelFailure(TEXT("level_actor_folder_path_invalid"), TEXT("Actor folder_path must be a string."),
          TEXT("folder_path"), Extension->TryGetField(TEXT("folder_path")), MakeShared<FJsonValueString>(TEXT("string")),
          Extension.Get(), TEXT("folder_path"));
      return false;
    }
    OutFolder = Value;
  }
  if (Extension->HasField(TEXT("transform"))) {
    const TSharedPtr<const FJsonObject> TransformObject =
        ReadObject(Extension, TEXT("transform"));
    FTransformPatch Patch;
    if (!TransformObject.IsValid()) {
      OutError = LevelFailure(TEXT("level_actor_transform_invalid"), TEXT("Actor transform must be an object."),
          TEXT("transform"), Extension->TryGetField(TEXT("transform")), MakeShared<FJsonValueString>(TEXT("transform object")),
          Extension.Get(), TEXT("transform"));
      return false;
    }
    if (!ParseTransformPatch(TransformObject, ETransformFrame::ActorWorld, Patch, OutError)) return false;
    OutTransform = MoveTemp(Patch);
  }
  if (Extension->HasField(TEXT("tags"))) {
    const TArray<TSharedPtr<FJsonValue>> *Values = nullptr;
    if (!Extension->TryGetArrayField(TEXT("tags"), Values) || !Values) {
      OutError = LevelFailure(TEXT("level_actor_tags_invalid"), TEXT("Actor tags must be an array of strings."),
          TEXT("tags"), Extension->TryGetField(TEXT("tags")), MakeShared<FJsonValueString>(TEXT("array of strings")),
          Extension.Get(), TEXT("tags"));
      return false;
    }
    OutTags.Emplace();
    for (int32 Index = 0; Index < Values->Num(); ++Index) {
      const TSharedPtr<FJsonValue>& Value = (*Values)[Index];
      FString TagText;
      if (!Value.IsValid() || !Value->TryGetString(TagText)) {
        OutError = LevelFailure(TEXT("level_actor_tag_invalid"), TEXT("Each actor tag must be a string."),
            TEXT("tags"), Value, MakeShared<FJsonValueString>(TEXT("string")), Extension.Get(), TEXT("tags"),
            FString::Printf(TEXT("/tags/%d"), Index));
        return false;
      }
      TagText.TrimStartAndEndInline();
      if (TagText.IsEmpty())
        continue;
      const FName Tag(*TagText);
      if (IsManagedActorIdentityTag(Tag)) {
        OutError = LevelFailure(TEXT("level_actor_managed_identity_tag_reserved"),
            TEXT("The actor tag uses the reserved managed-identity prefix; use the target managed_id field instead."),
            TEXT("tags"), Value, MakeShared<FJsonValueString>(TEXT("tag without the managed-identity prefix")),
            Extension.Get(), TEXT("tags"), FString::Printf(TEXT("/tags/%d"), Index));
        return false;
      }
      OutTags.GetValue().AddUnique(Tag);
    }
  }
  return true;
}

void ApplyActorTags(AActor& Actor, const TOptional<TArray<FName>>& Tags) {
  if (!Tags.IsSet()) return;
  Actor.Tags.RemoveAll([](const FName& Tag) { return !IsManagedActorIdentityTag(Tag); });
  for (const FName& Tag : Tags.GetValue()) Actor.Tags.AddUnique(Tag);
}

struct FPlaceableActorPatch {
  bool bPresent = false;
  FString Name;
  FString Id;
  FString Category;
  TMap<FString, FString> Select;
};

bool ParsePlaceableActorPatch(
    const TSharedPtr<const FJsonObject> &Extension,
    FPlaceableActorPatch &OutPatch, UE::UAI::Contract::FDiagnostic& OutError) {
  OutPatch = {};
  OutError.Reset();
  if (!Extension.IsValid() || !Extension->HasField(TEXT("candidate")))
    return true;
  const TSharedPtr<const FJsonObject> Candidate =
      ReadObject(Extension, TEXT("candidate"));
  if (!Candidate.IsValid()) {
    OutError = LevelFailure(TEXT("level_actor_candidate_invalid"), TEXT("Actor candidate must be an object."),
        TEXT("candidate"), Extension.IsValid() ? Extension->TryGetField(TEXT("candidate")) : nullptr,
        MakeShared<FJsonValueString>(TEXT("candidate object")), Extension.Get(), TEXT("candidate"));
    return false;
  }
  OutPatch.bPresent = true;
  UE::UAI::HjsonWorkspace::DomainAuthoring::FDomainHjsonCandidateSelector Selector;
  UE::UAI::HjsonWorkspace::FWorkspaceError SelectorError;
  if (!UE::UAI::HjsonWorkspace::DomainAuthoring::TryReadCandidateSelector(
          Candidate, TEXT("candidate"), Selector, SelectorError)) {
    OutError = SelectorError.GetDiagnostic();
    return false;
  }
  OutPatch.Select = MoveTemp(Selector.Select);
  Candidate->TryGetStringField(TEXT("name"), OutPatch.Name);
  Candidate->TryGetStringField(TEXT("id"), OutPatch.Id);
  OutPatch.Name.TrimStartAndEndInline();
  OutPatch.Id.TrimStartAndEndInline();
  if (OutPatch.Name.IsEmpty() && OutPatch.Id.IsEmpty()) {
    OutError = LevelFailure(TEXT("level_actor_candidate_selector_required"),
        TEXT("Actor candidate requires a non-empty name or id."), TEXT("candidate_selector_count"),
        MakeShared<FJsonValueNumber>(0),
        MakeShared<FJsonValueString>(TEXT("at least one non-empty name or id")), Candidate.Get());
    return false;
  }
  if (Extension->HasField(TEXT("placement_context"))) {
    const TSharedPtr<const FJsonObject> Context =
        ReadObject(Extension, TEXT("placement_context"));
    if (!Context.IsValid()) {
      OutError = LevelFailure(TEXT("level_actor_placement_context_invalid"), TEXT("placement_context must be an object."),
          TEXT("placement_context"), Extension->TryGetField(TEXT("placement_context")), MakeShared<FJsonValueString>(TEXT("object")),
          Extension.Get(), TEXT("placement_context"));
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
    UE::UAI::Contract::FDiagnostic& OutError) {
  const TSharedPtr<const FJsonObject> Candidate =
      ReadObject(Extension, TEXT("candidate"));
  if (!Candidate.IsValid()) {
    OutError = LevelFailure(TEXT("level_actor_candidate_invalid"), TEXT("Actor candidate must be an object."),
        TEXT("candidate"), Extension.IsValid() ? Extension->TryGetField(TEXT("candidate")) : nullptr,
        MakeShared<FJsonValueString>(TEXT("candidate object")), Extension.Get(), TEXT("candidate"));
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
      OutError = LevelFailure(TEXT("level_actor_candidate_assertion_mismatch"),
          FString::Printf(TEXT("Candidate assertion '%s' must match the resolved candidate."), Field), Field,
          Candidate->TryGetField(Field), MakeShared<FJsonValueString>(Resolved.AuthoringSelectors.FindRef(Selector)),
          Candidate.Get(), Field);
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
  TSharedPtr<const FJsonObject> Input;
  TOptional<FString> ParentComponent;
  TOptional<FString> SocketName;
  TOptional<FTransformPatch> Transform;
};

bool ParseAttachmentExtension(const TSharedPtr<const FJsonObject> &Extension,
                              FAttachmentPatch &OutPatch, UE::UAI::Contract::FDiagnostic& OutError) {
  OutPatch = FAttachmentPatch();
  OutError.Reset();
  if (!Extension.IsValid() || !Extension->HasField(TEXT("attachment")))
    return true;
  const TSharedPtr<const FJsonObject> Attachment =
      ReadObject(Extension, TEXT("attachment"));
  if (!Attachment.IsValid()) {
    OutError = LevelFailure(TEXT("level_component_attachment_invalid"), TEXT("Component attachment must be an object."),
        TEXT("attachment"), Extension->TryGetField(TEXT("attachment")), MakeShared<FJsonValueString>(TEXT("attachment object")),
        Extension.Get(), TEXT("attachment"));
    return false;
  }
  OutPatch.bPresent = true;
  OutPatch.Input = Attachment;
  if (Attachment->HasField(TEXT("parent_component"))) {
    FString Value;
    if (!Attachment->TryGetStringField(TEXT("parent_component"), Value)) {
      OutError = LevelFailure(TEXT("level_component_parent_invalid"), TEXT("Attachment parent_component must be a string."),
          TEXT("parent_component"), Attachment->TryGetField(TEXT("parent_component")), MakeShared<FJsonValueString>(TEXT("string")),
          Attachment.Get(), TEXT("parent_component"));
      return false;
    }
    OutPatch.ParentComponent = Value;
  }
  if (Attachment->HasField(TEXT("socket_name"))) {
    FString Value;
    if (!Attachment->TryGetStringField(TEXT("socket_name"), Value)) {
      OutError = LevelFailure(TEXT("level_component_socket_invalid"), TEXT("Attachment socket_name must be a string."),
          TEXT("socket_name"), Attachment->TryGetField(TEXT("socket_name")), MakeShared<FJsonValueString>(TEXT("string")),
          Attachment.Get(), TEXT("socket_name"));
      return false;
    }
    OutPatch.SocketName = Value;
  }
  if (!OutPatch.ParentComponent.IsSet() || !OutPatch.SocketName.IsSet()) {
    const TCHAR* Missing = !OutPatch.ParentComponent.IsSet() ? TEXT("parent_component") : TEXT("socket_name");
    OutError = LevelFailure(TEXT("level_component_attachment_patch_incomplete"),
        FString::Printf(TEXT("Component attachment requires '%s'; specify an empty string to detach or use no socket."), Missing),
        Missing, nullptr, MakeShared<FJsonValueString>(TEXT("string")), Attachment.Get(), Missing);
    return false;
  }
  if (Attachment->HasField(TEXT("transform"))) {
    const TSharedPtr<const FJsonObject> TransformObject =
        ReadObject(Attachment, TEXT("transform"));
    FTransformPatch Patch;
    if (!TransformObject.IsValid()) {
      OutError = LevelFailure(TEXT("level_component_transform_invalid"), TEXT("Component attachment transform must be an object."),
          TEXT("transform"), Attachment->TryGetField(TEXT("transform")), MakeShared<FJsonValueString>(TEXT("transform object")),
          Attachment.Get(), TEXT("transform"));
      return false;
    }
    if (!ParseTransformPatch(TransformObject, ETransformFrame::ComponentParentRelative, Patch, OutError)) return false;
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
  TSharedPtr<const FJsonObject> Input;
  FString ParentManagedId;
  FString SocketName;
};

bool ParseActorAttachmentExtension(
    const TSharedPtr<const FJsonObject> &Extension,
    FActorAttachmentPatch &OutPatch, UE::UAI::Contract::FDiagnostic& OutError) {
  OutPatch = FActorAttachmentPatch();
  OutError.Reset();
  if (!Extension.IsValid() || !Extension->HasField(TEXT("attachment")))
    return true;
  const TSharedPtr<const FJsonObject> Attachment =
      ReadObject(Extension, TEXT("attachment"));
  if (!Attachment.IsValid() ||
      !Attachment->TryGetStringField(TEXT("parent_managed_id"),
                                     OutPatch.ParentManagedId)) {
    OutError = LevelFailure(TEXT("level_actor_attachment_parent_managed_id_required"),
        TEXT("Actor attachment requires a string parent_managed_id; an empty string detaches the actor."),
        Attachment.IsValid() ? TEXT("parent_managed_id") : TEXT("attachment"),
        Attachment.IsValid() ? Attachment->TryGetField(TEXT("parent_managed_id")) : Extension->TryGetField(TEXT("attachment")),
        MakeShared<FJsonValueString>(Attachment.IsValid() ? TEXT("string") : TEXT("attachment object")),
        Attachment.IsValid() ? Attachment.Get() : Extension.Get(),
        Attachment.IsValid() ? TEXT("parent_managed_id") : TEXT("attachment"));
    return false;
  }
  if (Attachment->HasField(TEXT("socket_name")) &&
      !Attachment->TryGetStringField(TEXT("socket_name"),
                                     OutPatch.SocketName)) {
    OutError = LevelFailure(TEXT("level_actor_attachment_socket_invalid"), TEXT("Actor attachment socket_name must be a string."),
        TEXT("socket_name"), Attachment->TryGetField(TEXT("socket_name")), MakeShared<FJsonValueString>(TEXT("string")),
        Attachment.Get(), TEXT("socket_name"));
    return false;
  }
  OutPatch.Input = Attachment;
  OutPatch.ParentManagedId.TrimStartAndEndInline();
  OutPatch.SocketName.TrimStartAndEndInline();
  OutPatch.bPresent = true;
  return true;
}

bool IsUnchangedActorAttachment(const AActor* Actor, const FActorAttachmentPatch& Patch) {
  if (!Actor || !Patch.bPresent) return false;
  const AActor* Parent = Actor->GetAttachParentActor();
  const FString ParentId = Parent ? GetManagedActorId(*Parent) : FString();
  return ParentId.Equals(Patch.ParentManagedId, ESearchCase::IgnoreCase)
      && Actor->GetAttachParentSocketName() == FName(*Patch.SocketName);
}

bool ApplyActorAttachment(AActor &Actor, UWorld &World,
                          const FActorAttachmentPatch &Patch,
                          UE::UAI::Contract::FDiagnostic& OutError) {
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
  FScopedSourceLocations ParentSources(&ParentTarget.Get(), [&Patch](const FString& Pointer) {
    UE::UAI::Contract::FSourceLocation Source;
    if (Patch.Input.IsValid()) FScopedSourceLocations::Locate(*Patch.Input,
        Pointer == TEXT("/managed_id") ? TEXT("parent_managed_id") : FString(), Source);
    return Source;
  });
  AActor *Parent = FindActorExact(World, ParentTarget, OutError);
  if (!Parent) {
    if (OutError.IsEmpty())
      OutError = LevelFailure(TEXT("level_actor_attachment_parent_not_found"),
          TEXT("No actor matches the requested attachment parent managed id."), TEXT("matching_parent_count"),
          MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueNumber>(1), Patch.Input.Get(), TEXT("parent_managed_id"));
    return false;
  }
  if (Parent == &Actor) {
    OutError = LevelFailure(TEXT("level_actor_attachment_self_parent"), TEXT("An actor cannot be attached to itself."),
        TEXT("parent_is_self"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false),
        Patch.Input.Get(), TEXT("parent_managed_id"));
    OutError.CandidateName = Actor.GetActorLabel();
    return false;
  }
  if (!Actor.GetRootComponent() || !Parent->GetRootComponent()) {
    OutError = LevelFailure(TEXT("level_actor_attachment_requires_root_components"),
        !Actor.GetRootComponent() ? TEXT("The actor to attach has no root component.") : TEXT("The requested parent actor has no root component."),
        TEXT("root_component_present"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true),
        Patch.Input.Get(), TEXT("parent_managed_id"));
    OutError.CandidateName = !Actor.GetRootComponent() ? Actor.GetActorLabel() : Parent->GetActorLabel();
    return false;
  }
  if (!Actor.AttachToActor(
          Parent, FAttachmentTransformRules::KeepWorldTransform,
          Patch.SocketName.IsEmpty() ? NAME_None : FName(*Patch.SocketName))) {
    OutError = LevelFailure(TEXT("level_actor_attachment_apply_failed"),
        FString::Printf(TEXT("Unreal rejected attaching actor '%s' to '%s' at socket '%s'."), *Actor.GetActorLabel(), *Parent->GetActorLabel(), *Patch.SocketName),
        TEXT("attachment_succeeded"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Patch.Input.Get());
    OutError.CandidateName = Actor.GetActorLabel();
    return false;
  }
  return true;
}

void AddTransformOverrideContext(TMap<FString, FString> &Context,
                                 const TCHAR *Prefix,
                                 const FTransform &Transform) {
  const FString KeyPrefix(Prefix);
  // Display strings round vector/rotator components and are not value transport.
  const TSharedPtr<FJsonValue> Value = UeAgentPropertyValueCodec::EncodeStructValue(
      TBaseStructure<FTransform>::Get(), &Transform, nullptr, {});
  FString Encoded;
  FJsonSerializer::Serialize(Value->AsObject().ToSharedRef(),
                            TJsonWriterFactory<>::Create(&Encoded));
  Context.Add(KeyPrefix + TEXT("transform_value"), MoveTemp(Encoded));
}

bool ApplyComponentAttachment(UActorComponent &Component, AActor &Owner,
                              const FAttachmentPatch &Patch,
                              UE::UAI::Contract::FDiagnostic& OutError) {
  OutError.Reset();
  if (!Patch.bPresent)
    return true;
  USceneComponent *Scene = Cast<USceneComponent>(&Component);
  if (!Scene) {
    OutError = LevelFailure(TEXT("level_component_attachment_requires_scene_component"),
        TEXT("Only a SceneComponent can have a spatial attachment."), TEXT("component_class"),
        MakeShared<FJsonValueString>(Component.GetClass()->GetPathName()),
        MakeShared<FJsonValueString>(USceneComponent::StaticClass()->GetPathName()), Patch.Input.Get());
    OutError.CandidateName = Component.GetName();
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
      UE::UAI::Contract::FDiagnostic FindError;
      UActorComponent *Found = FindComponentExact(Owner, ParentName, FindError);
      Parent = Cast<USceneComponent>(Found);
      if (!Parent || Parent == Scene) {
        if (FindError.IsValid()) OutError = MoveTemp(FindError);
        else if (!Found) OutError = LevelFailure(TEXT("level_component_parent_not_found"),
            TEXT("No component on the owner matches the requested attachment parent."), TEXT("matching_parent_count"),
            MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueNumber>(1), Patch.Input.Get(), TEXT("parent_component"));
        else if (!Parent) OutError = LevelFailure(TEXT("level_component_parent_requires_scene_component"),
            TEXT("The requested parent component is not a SceneComponent."), TEXT("parent_component_class"),
            MakeShared<FJsonValueString>(Found->GetClass()->GetPathName()), MakeShared<FJsonValueString>(USceneComponent::StaticClass()->GetPathName()),
            Patch.Input.Get(), TEXT("parent_component"));
        else OutError = LevelFailure(TEXT("level_component_self_parent"), TEXT("A component cannot be attached to itself."),
            TEXT("parent_is_self"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false),
            Patch.Input.Get(), TEXT("parent_component"));
        if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = ParentName;
        if (Patch.Input.IsValid()) UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Patch.Input, TEXT("parent_component"));
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
      OutError = LevelFailure(TEXT("level_component_attach_failed"),
          FString::Printf(TEXT("Unreal rejected attaching component '%s' to '%s' at socket '%s'."), *Component.GetName(), *Parent->GetName(), *Socket.ToString()),
          TEXT("attachment_succeeded"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Patch.Input.Get());
      OutError.CandidateName = Component.GetName();
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

class FLevelPersistentContext {
public:
  explicit FLevelPersistentContext(UWorld& InWorld)
      : World(&InWorld), Path(InWorld.GetPathName()), PackageName(InWorld.GetPackage()->GetName()) {
    for (TActorIterator<ANavMeshBoundsVolume> It(&InWorld); It; ++It) {
      NavigationBoundsBefore.Add(*It, It->GetComponentsBoundingBox(true));
    }
  }
  TSet<const void*> Participants, Released;
  TMap<const void*, TFunction<void()>> ClearBindings;
  TSharedPtr<FLevelAuthoringScratchWorld> Scratch;
  TWeakObjectPtr<UWorld> World;
  FString Path, PackageName;
  TSet<TWeakObjectPtr<AActor>> SettledInputActors;
  TMap<TWeakObjectPtr<ANavMeshBoundsVolume>, FBox> NavigationBoundsBefore;
  TMap<FString, TWeakObjectPtr<UWorld>> TouchedLevelWorlds;
  TMap<FString, FString> MovedActorGuids, MovedActorPaths;
  void RecordMove(const FGuid& OldGuid, const FString& OldPath, const AActor& Actor) {
    const FString BeforeGuid = OldGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
    const FString AfterGuid = Actor.GetActorGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
    for (auto& Pair : MovedActorGuids) if (Pair.Value == BeforeGuid) Pair.Value = AfterGuid;
    for (auto& Pair : MovedActorPaths) if (Pair.Value == OldPath) Pair.Value = Actor.GetPathName();
    MovedActorGuids.Add(BeforeGuid, AfterGuid);
    MovedActorPaths.Add(OldPath, Actor.GetPathName());
  }
  AActor* FindActor(UWorld& InWorld, const TSharedPtr<const FJsonObject>& Target, UE::UAI::Contract::FDiagnostic& Error) const {
    auto Remapped = MakeShared<FJsonObject>();
    Remapped->Values = Target->Values;
    if (const FString* Guid = MovedActorGuids.Find(ReadString(Target, TEXT("actor_guid")))) Remapped->SetStringField(TEXT("actor_guid"), *Guid);
    if (const FString* PathValue = MovedActorPaths.Find(ReadString(Target, TEXT("object_path")))) Remapped->SetStringField(TEXT("object_path"), *PathValue);
    return FindActorExact(InWorld, Remapped, Error);
  }
  void TrackLevel(ULevel* Level) {
    if (UWorld* LevelWorld = Level ? Cast<UWorld>(Level->GetOuter()) : nullptr)
      TouchedLevelWorlds.Add(LevelWorld->GetPackage()->GetName(), LevelWorld);
  }
  bool BuildPackagePlans(TArray<FPackageDurablePlan>& Plans, FOperationFailure& Failure) const {
    if (!BuildPackageDurablePlan(Path, PackageName, World.Get(), EPackageDurableIntent::Save, Plans, Failure)) return false;
    for (const auto& Pair : TouchedLevelWorlds)
      if (Pair.Key != PackageName && Pair.Value.IsValid()
          && !BuildPackageDurablePlan(Pair.Value->GetPathName(), Pair.Key, Pair.Value.Get(), EPackageDurableIntent::Save, Plans, Failure)) return false;
    return true;
  }
  void PublishNavigationBounds(UNavigationSystemV1& Navigation) const {
    if (!World.IsValid()) return;
    for (TActorIterator<ANavMeshBoundsVolume> It(World.Get()); It; ++It) {
      const FBox Bounds = It->GetComponentsBoundingBox(true);
      const FBox* Before = NavigationBoundsBefore.Find(*It);
      if (!Before || !(*Before == Bounds)) {
        // Native registration captured the bounds before authoring finished.
        // Direct transform/brush edits do not emit the editor's actor-moved
        // event. Publish the final bounds without rerunning construction scripts.
        Navigation.OnNavigationBoundsUpdated(*It);
      }
    }
  }
  bool SettleInputs(AActor* Actor, FOperationFailure& Failure) {
    if (!Actor || SettledInputActors.Contains(Actor)) return true;
    UE::UAI::Contract::FDiagnostic Error;
    if (ABrush* Brush = Cast<ABrush>(Actor)) {
      if (Brush->IsStaticBrush() && Brush->GetLevel()
          && !UUeAgentBrushBuilderActorFactory::SettleLevel(*Brush->GetLevel(), Error)) {
        SetFailure(Failure, MoveTemp(Error));
        return false;
      }
    }
    if (!UeAgentPcgComponentAuthoring::SettleOwnedInputs(*Actor, Error)) {
      SetFailure(Failure, MoveTemp(Error));
      return false;
    }
    SettledInputActors.Add(Actor);
    return true;
  }
  bool Release(const void* State, FOperationFailure& Failure) {
    DiscardLevelCaptureReuse();
    Released.Add(State);
    if (Released.Num() != Participants.Num()) return true;
    // Every participant has released proposed/backup roots. The scratch world
    // can still retain native references (for example Brush -> original Model).
    if (Scratch.IsValid()) Scratch->ReleaseForPersistentReadback();
    UWorld* Current = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!Current) {
      SetFailure(Failure, PresenceFailure(TEXT("level_persistent_active_world_changed"),
          TEXT("There is no active Editor world to release for persistent readback."), TEXT("active_editor_world_present"), false, true, PackageName)); return false;
    }
    if (Current->GetPackage()->GetName() != PackageName) { World.Reset(); return true; }
    Current->GetPackage()->SetDirtyFlag(false);
    World.Reset();
    if (!UEditorLoadingAndSavingUtils::NewBlankMap(false)) {
      SetFailure(Failure, PresenceFailure(TEXT("level_persistent_detach_failed"),
          TEXT("Unreal could not open a blank map to release the saved world before readback."), TEXT("blank_map_opened"), false, true, PackageName)); return false;
    }
    return true;
  }
  bool Rebind(FOperationFailure& Failure) {
    if (Released.IsEmpty() && World.IsValid()) return true;
    UWorld* Current = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!Current || Current->GetPackage()->IsDirty()) {
      SetFailure(Failure, PresenceFailure(TEXT("level_persistent_current_world_dirty"),
          !Current ? TEXT("There is no active Editor world before persistent readback.")
              : TEXT("The current Editor world has unsaved changes and cannot be replaced for readback."),
          !Current ? TEXT("active_editor_world_present") : TEXT("package_dirty"), Current != nullptr, Current == nullptr, PackageName)); return false;
    }
    const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetMapPackageExtension());
    World = UEditorLoadingAndSavingUtils::LoadMap(Filename);
    if (!World.IsValid() || World->GetPathName() != Path) {
      auto Error = PresenceFailure(TEXT("level_persistent_reload_failed"),
          !World.IsValid() ? TEXT("Unreal could not reload the saved world for persistent readback.")
              : TEXT("The reloaded world does not match the saved world."),
          !World.IsValid() ? TEXT("world_loaded") : TEXT("world_path_matches"), false, true, PackageName);
      if (World.IsValid()) {
        Error.ActualEvidence = MakeShared<FJsonValueString>(World->GetPathName());
        Error.ExpectedEvidence = MakeShared<FJsonValueString>(Path);
      }
      SetFailure(Failure, MoveTemp(Error)); return false;
    }
    Released.Reset();
    return true;
  }
  void ReleaseReadbackBindings() {
    DiscardLevelCaptureReuse();
    for (const auto& Pair : ClearBindings) Pair.Value();
    // Isolated observation keeps the live world loaded, so Release() may not
    // have run. Prepared brush copies can still reference its original Model.
    // Release the same scratch owner before restoring a different Editor map.
    if (Scratch.IsValid()) Scratch->ReleaseForPersistentReadback();
    World.Reset();
  }
};

class FLevelPersistentState : public IReflectionOperationLifecycleState {
public:
  ~FLevelPersistentState() override {
    if (Persistent.IsValid()) { Persistent->Participants.Remove(this); Persistent->ClearBindings.Remove(this); }
  }
  void CaptureInputSources(const FPrepareRequest& Request) {
    DiagnosticInput = MakeShared<FJsonObject>();
    if (Request.Input->Target.IsValid())
      DiagnosticInput->SetObjectField(TEXT("target"), ConstCastSharedPtr<FJsonObject>(Request.Input->Target));
    if (Request.Input->ProviderExtension.IsValid())
      DiagnosticInput->SetObjectField(TEXT("provider_extension"), ConstCastSharedPtr<FJsonObject>(Request.Input->ProviderExtension));
    DiagnosticSources = FScopedSourceLocations::CaptureProjection(*DiagnosticInput);
  }
  void SetPersistentContext(const TSharedPtr<FLevelPersistentContext>& Context) {
    Persistent = Context;
    if (Persistent.IsValid()) {
      Persistent->Participants.Add(this);
      Persistent->ClearBindings.Add(this, [this] { ClearPersistentReferences(); });
    }
  }
  bool BuildDurablePackagePlans(TArray<FPackageDurablePlan>& Plans, FOperationFailure& Failure) const override {
    return Persistent.IsValid()
      ? Persistent->BuildPackagePlans(Plans, Failure)
      : IReflectionOperationLifecycleState::BuildDurablePackagePlans(Plans, Failure);
  }
  bool SupportsPersistentReadback() const override { return Persistent.IsValid(); }
  bool ReleaseForPersistentReadback(FOperationFailure& Failure) override {
    ClearPersistentReferences();
    if (!Persistent.IsValid()) {
      SetFailure(Failure, StatePresenceFailure(TEXT("level_persistent_context_missing"),
          TEXT("Persistent readback has no shared world context."), TEXT("persistent_context_present"), false, true));
      return false;
    }
    return Persistent->Release(this, Failure);
  }
  bool RebindAfterPersistentReadback(FOperationFailure& Failure) override {
    if (!Persistent.IsValid()) {
      SetFailure(Failure, StatePresenceFailure(TEXT("level_persistent_context_missing"),
          TEXT("Persistent readback has no shared world context to rebind."), TEXT("persistent_context_present"), false, true));
      return false;
    }
    return Persistent->Rebind(Failure) && BindPersistentWorld(*Persistent->World, false, Failure);
  }
  bool RebindAfterCheckpointRestore(FOperationFailure& Failure) override {
    if (!Persistent.IsValid()) {
      SetFailure(Failure, StatePresenceFailure(TEXT("level_persistent_context_missing"),
          TEXT("Checkpoint restore has no shared world context to rebind."), TEXT("persistent_context_present"), false, true));
      return false;
    }
    Persistent->MovedActorGuids.Reset(); Persistent->MovedActorPaths.Reset();
    return Persistent->Rebind(Failure) && BindPersistentWorld(*Persistent->World, true, Failure);
  }
protected:
  void SetFailure(FOperationFailure& OutFailure, FDiagnostic Diagnostic) const {
    if (Diagnostic.TargetPath.IsEmpty())
      Diagnostic.TargetPath = ReadString(ReadObject(DiagnosticInput, TEXT("target")), TEXT("world_path"));
    if (Diagnostic.Stage.IsEmpty()) Diagnostic.Stage = TEXT("level_apply");
    OutFailure.Diagnostic = MoveTemp(Diagnostic);
  }
  FDiagnostic StatePresenceFailure(const TCHAR* Code, const FString& Reason, const FString& Field,
      bool bActual, bool bExpected) const {
    const auto Target = ReadObject(DiagnosticInput, TEXT("target"));
    const auto Extension = ReadObject(DiagnosticInput, TEXT("provider_extension"));
    const FString Component = ReadString(Target, TEXT("component_name"));
    return PresenceFailure(Code, Reason, Field, bActual, bExpected, ReadString(Target, TEXT("world_path")),
        Component.IsEmpty() ? ReadString(Extension, TEXT("label")) : Component);
  }
  TSharedPtr<FJsonObject> DiagnosticInput;
  TMap<FString, UE::UAI::Contract::FSourceLocation> DiagnosticSources;
  virtual void ClearPersistentReferences() = 0;
  virtual bool BindPersistentWorld(UWorld&, bool, FOperationFailure&) = 0;
  TSharedPtr<FLevelPersistentContext> Persistent;
};

class FWorldLifecycleState final : public FLevelPersistentState {
public:
  bool PrepareTopology(UWorld& InWorld, const TSharedPtr<const FJsonObject>& Extension, FOperationFailure& Failure) {
    UE::UAI::Contract::FDiagnostic Error;
    if (!Topology.Prepare(InWorld, Extension, Error)) { SetFailure(Failure, MoveTemp(Error)); return false; }
    if (Topology.IsPresent() && Persistent.IsValid())
    {
      for (ULevel* Level : InWorld.GetLevels()) Persistent->TrackLevel(Level);
      // Load and checkpoint each existing destination map before any relation
      // or Actor mutation. Post-apply inventory growth cannot supply its before-image.
      for (const auto& Pair : Topology.GetDesired()->GetObjectField(TEXT("streaming_levels"))->Values) {
        UWorld* Map = LoadObject<UWorld>(nullptr, *(Pair.Key + TEXT(".") + FPackageName::GetShortName(Pair.Key)));
        if (Map) Persistent->TrackLevel(Map->PersistentLevel);
      }
    }
    return true;
  }
  FWorldLifecycleState(UWorld &InWorld,
                       TSharedRef<FLevelAuthoringScratchWorld> InScratchWorld,
                       AWorldSettings* InProposed,
                       UeAgentAuthoringIdentity::FCanonicalIdentity InIdentity)
      : World(&InWorld), ScratchWorld(MoveTemp(InScratchWorld)),
        Proposed(InProposed), Identity(MoveTemp(InIdentity)) {}

  virtual FString GetTargetKey() const override {
    return Identity.CanonicalKey;
  }
  virtual EOperation GetResolvedOperation() const override {
    return EOperation::Update;
  }
  virtual FReflectionRoot GetProposedRoot() const override {
    return FReflectionRoot::MakeObject(Proposed.IsValid() ? Proposed.Get()
        : World.IsValid() ? World->GetWorldSettings() : nullptr);
  }
  virtual FReflectionRoot GetActualRoot() const override {
    return FReflectionRoot::MakeObject(World.IsValid() ? World->GetWorldSettings() : nullptr);
  }
  virtual bool ShouldApplyPropertiesToActualRoot() const override {
    return true;
  }
  virtual bool CaptureRollback(FOperationFailure &OutFailure) override {
    OutFailure = {};
    return true;
  }
  virtual bool ApplyLifecycle(FOperationFailure &OutFailure) override {
    FScopedSourceLocations Sources(DiagnosticInput.Get(), [this](const FString& Pointer) {
      return FScopedSourceLocations::ResolveProjection(DiagnosticSources, Pointer);
    });
    OutFailure = {};
    if (Topology.IsPresent() && World.IsValid()) {
      UE::UAI::Contract::FDiagnostic Error;
      if (!Topology.Apply(*World, Error)) { SetFailure(OutFailure, MoveTemp(Error)); return false; }
      if (Persistent.IsValid()) for (ULevel* Level : World->GetLevels()) Persistent->TrackLevel(Level);
    }
    if (Proposed.IsValid() && World.IsValid()) {
      AWorldSettings* Settings = World->GetWorldSettings();
      if (!Settings) { SetFailure(OutFailure, StatePresenceFailure(TEXT("level_world_settings_missing"),
          TEXT("The world no longer has a WorldSettings object to update."), TEXT("world_settings_present"), false, true)); return false; }
      Settings->Modify();
      Settings->MarkPackageDirty();
    }
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
    if (!BuildRequests(OutRequests, OutFailure)) return false;
    if (Topology.IsPresent() && !OutRequests.IsEmpty()) {
      FString Json;
      FJsonSerializer::Serialize(Topology.GetDesired().ToSharedRef(), TJsonWriterFactory<>::Create(&Json));
      OutRequests[0].Context.Add(TEXT("world_topology_override"), Json);
    }
    return true;
  }
  virtual bool BuildDomainActualReadRequests(
      TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
      FOperationFailure &OutFailure) const override {
    return BuildRequests(OutRequests, OutFailure);
  }
  virtual bool Rollback(FOperationFailure &OutFailure) override {
    OutFailure = {};
    if (World.IsValid()) {
      UE::UAI::Contract::FDiagnostic Error;
      if (!Topology.Restore(*World, Error)) { SetFailure(OutFailure, MoveTemp(Error)); return false; }
    }
    return true;
  }

private:
  void ClearPersistentReferences() override { World.Reset(); Proposed.Reset(); Topology.Release(); }
  bool BindPersistentWorld(UWorld& Reloaded, bool, FOperationFailure&) override { World = &Reloaded; return true; }
  bool BuildRequests(TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
                     FOperationFailure &OutFailure) const {
    UE::UAI::Contract::FDiagnostic Error;
    if (!BuildWorldSnapshotReadRequests(
            World.Get(), Identity.GetSelector(TEXT("world_path")), false,
            OutRequests, Error)) {
      SetFailure(OutFailure, MoveTemp(Error));
      return false;
    }
    OutFailure = {};
    return true;
  }

  TWeakObjectPtr<UWorld> World;
  TSharedRef<FLevelAuthoringScratchWorld> ScratchWorld;
  TStrongObjectPtr<AWorldSettings> Proposed;
  UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
  FWorldTopologyEdit Topology;
};

class FActorLifecycleState final : public FLevelPersistentState {
public:
  bool PrepareLevel(const TSharedPtr<const FJsonObject>& Extension, FOperationFailure& Failure) {
    OriginalLevelPackage = Existing.IsValid() && Existing->GetLevel()
        ? Existing->GetLevel()->GetPackage()->GetName() : World->GetPackage()->GetName();
    TargetLevelPackage = OriginalLevelPackage;
    if (Extension.IsValid() && Extension->HasField(TEXT("level"))) {
      if (!Extension->TryGetStringField(TEXT("level"), TargetLevelPackage)
          || !FPackageName::IsValidLongPackageName(TargetLevelPackage)
          || (World->GetWorldPartition() && TargetLevelPackage != World->GetPackage()->GetName())) {
        SetFailure(Failure, LevelFailure(TEXT("level_actor_level_invalid"),
            TEXT("Actor level must name an associated map package; partitioned Actors remain in their persistent map."),
            TEXT("level"), Extension->TryGetField(TEXT("level")), MakeShared<FJsonValueString>(TEXT("map package path")), Extension.Get()));
        return false;
      }
    }
    if (Persistent.IsValid()) {
      if (Existing.IsValid()) Persistent->TrackLevel(Existing->GetLevel());
      Persistent->TrackLevel(FindAuthoringLevel(*World, TargetLevelPackage));
    }
    return true;
  }
  FActorLifecycleState(UWorld &InWorld,
                       TSharedRef<FLevelAuthoringScratchWorld> InScratchWorld,
                       EOperation InOperation,
                       UeAgentAuthoringIdentity::FCanonicalIdentity InIdentity,
                       AActor *InExisting, AActor *InProposed,
                       FString InBaseLabel, FString InBaseFolder,
                       FTransform InBaseTransform, TOptional<FString> InLabel,
                       TOptional<FString> InFolder,
                       TOptional<FTransformPatch> InTransform,
	                       TOptional<TArray<FName>> InTags,
	                       FActorAttachmentPatch InAttachment,
	                       FPlaceableActorPatch InPlaceableCandidate,
                       bool InPreserveDataLayerPreview)
      : World(&InWorld), ScratchWorld(MoveTemp(InScratchWorld)),
        Operation(InOperation), Identity(MoveTemp(InIdentity)),
        Existing(InExisting), Proposed(InProposed),
        BaseLabel(MoveTemp(InBaseLabel)), BaseFolder(MoveTemp(InBaseFolder)),
        BaseTransform(MoveTemp(InBaseTransform)), Label(MoveTemp(InLabel)),
        Folder(MoveTemp(InFolder)), Transform(MoveTemp(InTransform)),
	        Tags(MoveTemp(InTags)),
	        Attachment(MoveTemp(InAttachment)),
	        PlaceableCandidate(MoveTemp(InPlaceableCandidate)),
        bPreserveDataLayerPreview(InPreserveDataLayerPreview) {}

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

  virtual bool ShouldNotifyProposedProperties() const override {
    // The inactive scratch actor has no partition/external package. Native
    // PostEditChange would therefore discard valid DataLayerAssets. Keep the
    // reflected preview intent; the real actor still receives native callbacks
    // and the shared transaction verifies its saved/reloaded state.
    return !bPreserveDataLayerPreview;
  }

  virtual bool CaptureRollback(FOperationFailure &OutFailure) override {
    OutFailure = {};
    if (!Existing.IsValid())
      return true;
    if (Operation == EOperation::Remove) return true;
    UE::UAI::Contract::FDiagnostic Error;
    Backup.Reset(ScratchWorld->DuplicateActor(*Existing, Error));
    if (!Backup.IsValid()) {
      SetFailure(OutFailure, MoveTemp(Error));
      return false;
    }
    OriginalAttachParent = Existing->GetAttachParentActor();
    OriginalAttachSocket = Existing->GetAttachParentSocketName();
    return true;
  }

  virtual bool ApplyProposed(FOperationFailure &OutFailure) override {
    FScopedSourceLocations Sources(DiagnosticInput.Get(), [this](const FString& Pointer) {
      return FScopedSourceLocations::ResolveProjection(DiagnosticSources, Pointer);
    });
    OutFailure = {};
    if (Operation == EOperation::Remove)
      return true;
    if (!Proposed.IsValid()) {
      SetFailure(OutFailure, StatePresenceFailure(TEXT("level_actor_proposed_missing"),
          TEXT("The prepared actor is no longer available for application."), TEXT("proposed_actor_present"), false, true));
      return false;
    }
    return true;
  }

  virtual bool ApplyLifecycle(FOperationFailure &OutFailure) override {
    FScopedSourceLocations Sources(DiagnosticInput.Get(), [this](const FString& Pointer) {
      return FScopedSourceLocations::ResolveProjection(DiagnosticSources, Pointer);
    });
    OutFailure = {};
    if (!World.IsValid()) {
      SetFailure(OutFailure, StatePresenceFailure(TEXT("level_world_not_found"),
          TEXT("The target world is no longer available for actor application."), TEXT("world_present"), false, true));
      return false;
    }
    if (Operation == EOperation::Remove) {
      UE::UAI::Contract::FDiagnostic Error;
      if (!Existing.IsValid()) {
        SetFailure(OutFailure, StatePresenceFailure(TEXT("level_actor_remove_failed"),
            TEXT("The actor to remove is no longer available."), TEXT("actor_present"), false, true));
        return false;
      }
      if (!Deletion.Apply(*World, {Existing.Get()}, Error)) {
        SetFailure(OutFailure, MoveTemp(Error));
        return false;
      }
      return true;
    }
    if (Operation == EOperation::Update) {
      Actual = Existing.Get();
      if (!Actual) {
        SetFailure(OutFailure, StatePresenceFailure(TEXT("level_actor_not_found"),
            TEXT("The actor to update is no longer available."), TEXT("actor_present"), false, true));
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
	          SetFailure(OutFailure, StatePresenceFailure(TEXT("level_actor_persistent_level_missing"),
              TEXT("The target world has no persistent level for actor placement."), TEXT("persistent_level_present"), false, true));
	          return false;
	        }
	        UeAgentPaletteSearch::FCandidate ResolvedCandidate;
	        UE::UAI::Contract::FDiagnostic CandidateError;
	        if (!UeAgentPaletteSearch::TryPerformPlaceableActorCandidate(
	                PlaceableCandidate.Name, PlaceableCandidate.Id,
	                PlaceableCandidate.Category, *World->PersistentLevel,
	                Proposed->GetActorTransform(), Params, Actual,
	                ResolvedCandidate, CandidateError, PlaceableCandidate.Select)) {
	          const auto CandidateInput = ReadObject(ReadObject(DiagnosticInput, TEXT("provider_extension")), TEXT("candidate"));
          if (CandidateInput.IsValid()) UeAgentAuthoringDiagnostics::LocateAuthoringSource(CandidateError, *CandidateInput);
          SetFailure(OutFailure, MoveTemp(CandidateError));
	          return false;
	        }
	      } else {
	        Actual = World->SpawnActor<AActor>(Proposed->GetClass(),
	                                           Proposed->GetActorTransform(), Params);
	      }
      if (!Actual) {
        auto Error = StatePresenceFailure(TEXT("level_actor_spawn_failed"),
            TEXT("Unreal could not spawn the prepared actor in the target world."), TEXT("actor_created"), false, true);
        Error.ProposedValue = MakeShared<FJsonValueString>(Proposed->GetClass()->GetPathName());
        SetFailure(OutFailure, MoveTemp(Error));
        return false;
      }
      const FString ManagedId = Identity.GetSelector(TEXT("managed_id"));
      if (!ManagedId.IsEmpty())
        Actual->Tags.AddUnique(FName(*MakeManagedActorTag(ManagedId)));
      ApplyActorExtension(*Actual, Label, Folder, Transform);
    }
    ULevel* Destination = FindAuthoringLevel(*World, TargetLevelPackage);
    if (!Destination) {
      SetFailure(OutFailure, LevelFailure(TEXT("level_actor_destination_unavailable"),
          TEXT("The destination level is not associated and loaded in this world."), TEXT("level"),
          MakeShared<FJsonValueString>(TargetLevelPackage), MakeShared<FJsonValueString>(TEXT("loaded associated map"))));
      return false;
    }
    if (Persistent.IsValid()) { Persistent->TrackLevel(Actual->GetLevel()); Persistent->TrackLevel(Destination); }
    if (Actual->GetLevel() != Destination) {
      const FGuid Guid = Actual->GetActorGuid();
      const FString OldPath = Actual->GetPathName();
      const FTransform WorldTransform = Actual->GetActorTransform();
      TArray<AActor*> Moved;
      if (UEditorLevelUtils::MoveActorsToLevel({Actual}, Destination, false, false, true, &Moved) != 1 || Moved.Num() != 1) {
        SetFailure(OutFailure, StatePresenceFailure(TEXT("level_actor_move_failed"),
            TEXT("Unreal did not move the actor to its destination level."), TEXT("actor_moved"), false, true)); return false;
      }
      Actual = Moved[0];
      if (Persistent.IsValid()) Persistent->RecordMove(Guid, OldPath, *Actual);
      Actual->SetActorTransform(WorldTransform);
      if (Operation == EOperation::Update) Existing.Reset(Actual);
    }
    ApplyActorTags(*Actual, Tags);
    UE::UAI::Contract::FDiagnostic AttachmentError;
    if (!ApplyActorAttachment(*Actual, *World, Attachment, AttachmentError)) {
      SetFailure(OutFailure, PublicLookupFailure(AttachmentError, EAuthorabilityPublicTarget::Actor));
      return false;
    }
    Actual->PostEditChange();
    Actual->MarkPackageDirty();
    return true;
  }

  virtual bool Compile(FOperationFailure &OutFailure) override {
    OutFailure = {};
    return !Persistent.IsValid() || Persistent->SettleInputs(
        Operation == EOperation::Remove ? nullptr : Actual, OutFailure);
  }
  virtual bool Save(FOperationFailure &OutFailure) override {
    OutFailure = {};
    return true;
  }
  virtual bool BuildDomainExpectedReadRequests(
      TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
      FOperationFailure &OutFailure) const override {
    if (!BuildRequests(
            Operation == EOperation::Remove
                ? nullptr
                : Proposed.Get(),
            Operation == EOperation::Remove, OutRequests, OutFailure)) {
      return false;
    }
    if (Operation != EOperation::Remove && !OutRequests.IsEmpty()) {
      TMap<FString, FString> &Context = OutRequests[0].Context;
      Context.Add(TEXT("actor_level_override"), TargetLevelPackage);
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
      AddCandidateTransformProjectionContext(Context);
      if (Attachment.bPresent) {
        Context.Add(TEXT("actor_attachment_override"), TEXT("true"));
        Context.Add(TEXT("actor_parent_managed_id_override"),
                    Attachment.ParentManagedId);
        Context.Add(TEXT("actor_socket_name_override"),
                    Attachment.SocketName);
      } else if (Operation == EOperation::Update && Existing.IsValid()) {
        // Scratch actors do not retain the live attachment graph. All other
        // expected fields come from the edited proposed actor, not the old one.
        const AActor* Parent = Existing->GetAttachParentActor();
        Context.Add(TEXT("actor_attachment_override"), TEXT("true"));
        Context.Add(TEXT("actor_parent_managed_id_override"), Parent ? GetManagedActorId(*Parent) : FString());
        Context.Add(TEXT("actor_socket_name_override"), Existing->GetAttachParentSocketName().IsNone()
            ? FString() : Existing->GetAttachParentSocketName().ToString());
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
    if (!OutRequests.IsEmpty()) {
      AddCandidateTransformProjectionContext(OutRequests[0].Context);
    }
    return true;
  }
  virtual bool BuildDomainBeforeReadRequests(
      TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
      FOperationFailure& OutFailure) const override {
    AActor* Owner = Actual ? Actual : Existing.Get();
    return BuildRequests(Owner, !Owner, OutRequests, OutFailure);
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
      SetFailure(OutFailure, StatePresenceFailure(TEXT("level_world_not_found"),
          TEXT("The target world is no longer available for actor rollback."), TEXT("world_present"), false, true));
      return false;
    }
    if (Operation == EOperation::Create || Operation == EOperation::Clone) {
      const bool bRebuildBrush = Actual && Actual->IsA<ABrush>();
      if (Actual && !World->DestroyActor(Actual, true, true)) {
        SetFailure(OutFailure, StatePresenceFailure(TEXT("level_actor_create_rollback_failed"),
            TEXT("Unreal could not destroy the newly created actor during rollback."), TEXT("actor_destroyed"), false, true));
        return false;
      }
      Actual = nullptr;
      if (bRebuildBrush && World->PersistentLevel) {
        UE::UAI::Contract::FDiagnostic Error;
        if (!UUeAgentBrushBuilderActorFactory::SettleLevel(*World->PersistentLevel, Error)) {
          SetFailure(OutFailure, MoveTemp(Error));
          return false;
        }
      }
      return true;
    }
    if (Operation == EOperation::Update && Existing.IsValid() &&
        Backup.IsValid()) {
      if (Existing->GetLevel()->GetPackage()->GetName() != OriginalLevelPackage) {
        ULevel* Original = FindAuthoringLevel(*World, OriginalLevelPackage);
        const FGuid Guid = Existing->GetActorGuid();
        const FString OldPath = Existing->GetPathName();
        TArray<AActor*> Moved;
        if (!Original || UEditorLevelUtils::MoveActorsToLevel({Existing.Get()}, Original, false, false, true, &Moved) != 1 || Moved.Num() != 1) {
          SetFailure(OutFailure, StatePresenceFailure(TEXT("level_actor_move_rollback_failed"),
              TEXT("Unreal could not restore the actor's original level."), TEXT("actor_level_restored"), false, true)); return false;
        }
        Existing.Reset(Moved[0]);
        if (Persistent.IsValid()) Persistent->RecordMove(Guid, OldPath, *Existing);
      }
      Existing->Tags = Backup->Tags;
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
      if (Actual->IsA<ABrush>() && Actual->GetLevel()) {
        UE::UAI::Contract::FDiagnostic Error;
        if (!UUeAgentBrushBuilderActorFactory::SettleLevel(*Actual->GetLevel(), Error)) {
          SetFailure(OutFailure, MoveTemp(Error));
          return false;
        }
      }
      return true;
    }
    if (Operation == EOperation::Remove) {
      UE::UAI::Contract::FDiagnostic Error;
      if (!Deletion.Restore(Error)) {
        SetFailure(OutFailure, MoveTemp(Error)); return false;
      }
      const auto Target = MakeShared<FJsonObject>();
      UeAgentAuthoringIdentity::WriteSelectorsFlat(Identity, Target);
      Actual = FindActorExact(*World, Target, Error);
      Existing.Reset(Actual);
      if (!Actual) {
        SetFailure(OutFailure, Error.IsValid() ? MoveTemp(Error) : StatePresenceFailure(TEXT("level_actor_remove_rollback_failed"),
            TEXT("The removed actor was not found after rollback restored its deletion record."), TEXT("restored_actor_present"), false, true));
        return false;
      }
    }
    return true;
  }

private:
  void AddCandidateTransformProjectionContext(
      TMap<FString, FString> &Context) const {
    if (Operation != EOperation::Create || !PlaceableCandidate.bPresent)
      return;
    if (!Transform.IsSet() || !Transform.GetValue().Translation.IsSet())
      Context.Add(TEXT("actor_transform_translation_omit"), TEXT("true"));
    if (!Transform.IsSet() || !Transform.GetValue().Rotation.IsSet())
      Context.Add(TEXT("actor_transform_rotation_omit"), TEXT("true"));
    if (!Transform.IsSet() || !Transform.GetValue().Scale.IsSet())
      Context.Add(TEXT("actor_transform_scale_omit"), TEXT("true"));
  }

  bool BuildRequests(AActor *Actor, const bool bExpectedAbsent,
                     TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
                     FOperationFailure &OutFailure) const {
    UE::UAI::Contract::FDiagnostic Error;
    if (!BuildActorSnapshotReadRequests(Actor, Identity, bExpectedAbsent,
                                        OutRequests, Error)) {
      SetFailure(OutFailure, MoveTemp(Error));
      return false;
    }
    OutFailure = {};
    return true;
  }

  void ClearPersistentReferences() override {
    Deletion.Release();
    World.Reset(); Existing.Reset(); Proposed.Reset(); Backup.Reset(); Actual = nullptr;
    OriginalAttachParent.Reset();
  }
  bool BindPersistentWorld(UWorld& Reloaded, bool bBefore, FOperationFailure& Failure) override {
    World = &Reloaded;
    const auto Target = MakeShared<FJsonObject>();
    UeAgentAuthoringIdentity::WriteSelectorsFlat(Identity, Target);
    UE::UAI::Contract::FDiagnostic Error;
    Actual = Persistent.IsValid() ? Persistent->FindActor(Reloaded, Target, Error) : FindActorExact(Reloaded, Target, Error);
    Existing.Reset(Actual);
    if (Error.IsValid()) { SetFailure(Failure, MoveTemp(Error)); return false; }
    const bool bAbsent = bBefore ? (Operation == EOperation::Create || Operation == EOperation::Clone) : Operation == EOperation::Remove;
    if ((Actual == nullptr) != bAbsent) {
      auto RebindError = StatePresenceFailure(TEXT("level_actor_persistent_rebind_failed"),
          bAbsent ? TEXT("The actor is still present after reloading a state that requires its absence.")
              : TEXT("The actor is missing after reloading a state that requires its presence."),
          TEXT("actor_present"), Actual != nullptr, !bAbsent);
      RebindError.ExpectedEvidence = MakeShared<FJsonValueObject>(Target);
      auto Observed = MakeShared<FJsonObject>();
      for (TActorIterator<AActor> It(&Reloaded); It; ++It)
        if ((*It)->GetActorLabel() == (Label.IsSet() ? Label.GetValue() : BaseLabel))
          Observed->SetStringField((*It)->GetPathName(), (*It)->GetActorGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
      RebindError.ActualEvidence = MakeShared<FJsonValueObject>(Observed);
      SetFailure(Failure, MoveTemp(RebindError));
      return false;
    }
    return true;
  }

  TWeakObjectPtr<UWorld> World;
  TSharedRef<FLevelAuthoringScratchWorld> ScratchWorld;
  EOperation Operation = EOperation::Update;
  UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
  TStrongObjectPtr<AActor> Existing;
  TStrongObjectPtr<AActor> Proposed;
  TStrongObjectPtr<AActor> Backup;
  FActorDeletion Deletion;
  FString TargetLevelPackage, OriginalLevelPackage;
  AActor *Actual = nullptr;
  TWeakObjectPtr<AActor> OriginalAttachParent;
  FName OriginalAttachSocket;
  FString BaseLabel;
  FString BaseFolder;
  FTransform BaseTransform = FTransform::Identity;
  TOptional<FString> Label;
  TOptional<FString> Folder;
  TOptional<FTransformPatch> Transform;
  TOptional<TArray<FName>> Tags;
  FActorAttachmentPatch Attachment;
	  FPlaceableActorPatch PlaceableCandidate;
  bool bPreserveDataLayerPreview = false;
};

class FComponentLifecycleState final
    : public FLevelPersistentState {
public:
  FComponentLifecycleState(
      UWorld &InWorld, TSharedRef<FLevelAuthoringScratchWorld> InScratchWorld,
      EOperation InRequestedOperation, EOperation InOperation,
      bool bInAdoptMatchingComponentFromSpawnedOwner,
      UeAgentAuthoringIdentity::FCanonicalIdentity InIdentity,
      TSharedPtr<const FJsonObject> InOwnerTarget, UActorComponent *InExisting,
      UActorComponent *InProposed, FAttachmentPatch InAttachment,
      TSharedPtr<const FJsonObject> InPcg)
      : World(&InWorld), ScratchWorld(MoveTemp(InScratchWorld)),
        RequestedOperation(InRequestedOperation), Operation(InOperation),
        bAdoptMatchingComponentFromSpawnedOwner(
            bInAdoptMatchingComponentFromSpawnedOwner),
        Identity(MoveTemp(InIdentity)), OwnerTarget(MoveTemp(InOwnerTarget)),
        Existing(InExisting), Proposed(InProposed),
        Attachment(MoveTemp(InAttachment)), Pcg(MoveTemp(InPcg)) {}

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
        Operation == EOperation::Remove ? nullptr : ResolveCurrentComponent());
  }
  virtual bool ShouldApplyPropertiesToActualRoot() const override {
    return true;
  }

  bool RequiresCheckpointRollback() const override {
    // Native property notifications can replace construction-script components.
    // Their prepared property addresses no longer identify the live instance.
    return Persistent.IsValid() && Existing.IsValid()
        && Existing->IsCreatedByConstructionScript();
  }

  void ConfigurePropertyOptions(UeAgentAuthoringObjectApply::FOptions& Options) const override {
    const UActorComponent* Component = Existing.IsValid() ? Existing.Get() : Proposed.Get();
    if (Component && Component->GetOwner() && Component->GetOwner()->IsA<AWorldPartitionHLOD>()) {
      Options.IncludeRootProperty = [](const FProperty* Property) {
        return Property && Property->GetFName() != TEXT("bVisible");
      };
    }
    // Per-field post-edit reconstructs SCS components. Subsequent prepared
    // addresses would still refer to the retired component instance.
    if (Existing.IsValid() && Existing->IsCreatedByConstructionScript()) {
      Options.bWholeObjectNotification = true;
    }
  }

  virtual bool CaptureRollback(FOperationFailure &OutFailure) override {
    OutFailure = {};
    if (!Existing.IsValid())
      return true;
    UE::UAI::Contract::FDiagnostic Error;
    Backup.Reset(ScratchWorld->DuplicateComponent(*Existing, Error));
    if (!Backup.IsValid()) {
      SetFailure(OutFailure, MoveTemp(Error));
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
    FScopedSourceLocations Sources(DiagnosticInput.Get(), [this](const FString& Pointer) {
      return FScopedSourceLocations::ResolveProjection(DiagnosticSources, Pointer);
    });
    OutFailure = {};
    if (Operation == EOperation::Remove)
      return true;
    if (!Proposed.IsValid()) {
      SetFailure(OutFailure, StatePresenceFailure(TEXT("level_component_proposed_missing"),
          TEXT("The prepared component is no longer available for application."), TEXT("proposed_component_present"), false, true));
      return false;
    }
    if (Attachment.Transform.IsSet()) {
      if (!Cast<USceneComponent>(Proposed.Get())) {
        auto Error = LevelFailure(TEXT("level_component_transform_requires_scene_component"),
            TEXT("An attachment transform requires a SceneComponent."), TEXT("component_class"),
            MakeShared<FJsonValueString>(Proposed->GetClass()->GetPathName()),
            MakeShared<FJsonValueString>(USceneComponent::StaticClass()->GetPathName()), Attachment.Input.Get(), TEXT("transform"));
        Error.CandidateName = Proposed->GetName();
        SetFailure(OutFailure, MoveTemp(Error));
        return false;
      }
    }
    UE::UAI::Contract::FDiagnostic PcgError;
    if (!UeAgentPcgComponentAuthoring::Apply(*Proposed, Pcg, PcgError,
            ReadString(ReadObject(DiagnosticInput, TEXT("target")), TEXT("world_path")))) {
      if (PcgError.CandidateName.IsEmpty()) PcgError.CandidateName = Identity.GetSelector(TEXT("component_name"));
      SetFailure(OutFailure, MoveTemp(PcgError));
      return false;
    }
    return true;
  }

  virtual bool ApplyLifecycle(FOperationFailure &OutFailure) override {
    FScopedSourceLocations Sources(DiagnosticInput.Get(), [this](const FString& Pointer) {
      return FScopedSourceLocations::ResolveProjection(DiagnosticSources, Pointer);
    });
    OutFailure = {};
    if (!World.IsValid()) {
      SetFailure(OutFailure, StatePresenceFailure(TEXT("level_world_not_found"),
          TEXT("The target world is no longer available for component application."), TEXT("world_present"), false, true));
      return false;
    }
    const auto OriginalTarget = ReadObject(DiagnosticInput, TEXT("target"));
    FDerivedTargetSources OwnerSources(OwnerTarget.Get(), OriginalTarget.Get(), {
        {TEXT("/world_path"), TEXT("world_path")}, {TEXT("/actor_guid"), TEXT("owner_actor_guid")},
        {TEXT("/managed_id"), TEXT("owner_managed_id")}});
    UE::UAI::Contract::FDiagnostic OwnerError;
    AActor *Owner = Persistent.IsValid() ? Persistent->FindActor(*World, OwnerTarget, OwnerError) : FindActorExact(*World, OwnerTarget, OwnerError);
    if (!Owner) {
      if (OwnerError.IsEmpty()) OwnerError = StatePresenceFailure(TEXT("level_component_owner_not_found"),
          TEXT("The component owner actor is no longer available for application."), TEXT("owner_actor_present"), false, true);
      SetFailure(OutFailure, PublicLookupFailure(OwnerError, EAuthorabilityPublicTarget::Component));
      return false;
    }
    if (Operation == EOperation::Remove) {
      if (!Existing.IsValid()) {
        UE::UAI::Contract::FDiagnostic FindError;
        Existing.Reset(FindComponentExact(
            *Owner, Identity.GetSelector(TEXT("component_name")), FindError));
        if (!Existing.IsValid() && !FindError.IsEmpty()) {
          if (OriginalTarget.IsValid()) UeAgentAuthoringDiagnostics::LocateAuthoringSource(FindError, *OriginalTarget, TEXT("component_name"));
          SetFailure(OutFailure, PublicLookupFailure(FindError, EAuthorabilityPublicTarget::Component));
          return false;
        }
      }
      if (!Existing.IsValid()) {
        SetFailure(OutFailure, StatePresenceFailure(TEXT("level_component_not_found"),
            TEXT("The component to remove was not found on its owner."), TEXT("component_present"), false, true));
        return false;
      }
      const FName RetiredName = MakeUniqueObjectName(
          Owner, Existing->GetClass(),
          FName(*(OriginalName.ToString() + TEXT("_UAI_Removed"))));
      if (!Existing->Rename(
              *RetiredName.ToString(), Owner,
              REN_DontCreateRedirectors | REN_DoNotDirty |
                  REN_NonTransactional)) {
        SetFailure(OutFailure, StatePresenceFailure(TEXT("level_component_remove_failed"),
            TEXT("Unreal could not rename the component before removing it."), TEXT("component_renamed"), false, true));
        return false;
      }
      Existing->Modify();
      Existing->DestroyComponent();
      Existing.Reset();
      return true;
    }

    UE::UAI::Contract::FDiagnostic FindError;
    UActorComponent *Current = FindComponentExact(
        *Owner, Identity.GetSelector(TEXT("component_name")), FindError);
    if (!FindError.IsEmpty()) {
      if (OriginalTarget.IsValid()) UeAgentAuthoringDiagnostics::LocateAuthoringSource(FindError, *OriginalTarget, TEXT("component_name"));
      SetFailure(OutFailure, PublicLookupFailure(FindError, EAuthorabilityPublicTarget::Component));
      return false;
    }
    if (Operation == EOperation::Create && Current &&
        bAdoptMatchingComponentFromSpawnedOwner) {
      if (Current->GetClass() != Proposed->GetClass()) {
        auto Error = LevelFailure(TEXT("level_component_class_conflict"),
            TEXT("The spawned owner's existing component has a different class from the prepared component."), TEXT("component_class"),
            MakeShared<FJsonValueString>(Current->GetClass()->GetPathName()), MakeShared<FJsonValueString>(Proposed->GetClass()->GetPathName()));
        Error.CandidateName = Current->GetName();
        SetFailure(OutFailure, MoveTemp(Error));
        return false;
      }
      if (RequestedOperation == EOperation::Upsert) {
        Operation = EOperation::Update;
        Existing.Reset(Current);
      }
      Actual = Current;
      Actual->Modify();
    } else if (Operation == EOperation::Update) {
      Actual = Current ? Current : Existing.Get();
      if (!Actual) {
        SetFailure(OutFailure, StatePresenceFailure(TEXT("level_component_not_found"),
            TEXT("The component to update is no longer available."), TEXT("component_present"), false, true));
        return false;
      }
      Actual->Modify();
    } else {
      if (Current) {
        SetFailure(OutFailure, StatePresenceFailure(TEXT("level_component_already_exists"),
            TEXT("The owner already has a component with the requested name."), TEXT("component_present"), true, false));
        return false;
      }
      Actual = NewObject<UActorComponent>(
          Owner, Proposed->GetClass(),
          FName(*Identity.GetSelector(TEXT("component_name"))),
          RF_Transactional, Proposed.Get());
      if (!Actual) {
        auto Error = StatePresenceFailure(TEXT("level_component_create_failed"),
            TEXT("Unreal could not create the prepared component on its owner."), TEXT("component_created"), false, true);
        Error.ProposedValue = MakeShared<FJsonValueString>(Proposed->GetClass()->GetPathName());
        SetFailure(OutFailure, MoveTemp(Error));
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
    UE::UAI::Contract::FDiagnostic AttachmentError;
    if (!UeAgentPcgComponentAuthoring::Apply(*Actual, Pcg, AttachmentError,
            ReadString(ReadObject(DiagnosticInput, TEXT("target")), TEXT("world_path")))) {
      if (AttachmentError.CandidateName.IsEmpty()) AttachmentError.CandidateName = Identity.GetSelector(TEXT("component_name"));
      SetFailure(OutFailure, MoveTemp(AttachmentError));
      return false;
    }
    if (!ApplyComponentAttachment(*Actual, *Owner, Attachment,
                                  AttachmentError)) {
      SetFailure(OutFailure, PublicLookupFailure(AttachmentError, EAuthorabilityPublicTarget::Component));
      return false;
    }
    if (Operation != EOperation::Update || Attachment.bPresent || Pcg.IsValid()) {
      Actual->PostEditChange();
      Owner->MarkPackageDirty();
    }
    return true;
  }

  virtual bool Compile(FOperationFailure &OutFailure) override {
    OutFailure = {};
    if (!Persistent.IsValid() || !World.IsValid()) return true;
    UE::UAI::Contract::FDiagnostic Error;
    AActor* Owner = Persistent.IsValid() ? Persistent->FindActor(*World, OwnerTarget, Error) : FindActorExact(*World, OwnerTarget, Error);
    if (Error.IsValid()) { SetFailure(OutFailure, PublicLookupFailure(Error, EAuthorabilityPublicTarget::Component)); return false; }
    return Persistent->SettleInputs(Owner, OutFailure);
  }
  virtual bool Save(FOperationFailure &OutFailure) override {
    OutFailure = {};
    return true;
  }
  virtual bool BuildDomainExpectedReadRequests(
      TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
      FOperationFailure &OutFailure) const override {
    const bool bPropertyOnlyUpdate =
        Operation == EOperation::Update && !Attachment.bPresent && !Pcg.IsValid();
    FDiagnostic LookupError;
    UActorComponent* Expected = Operation == EOperation::Remove ? nullptr
        : bPropertyOnlyUpdate ? ResolveCurrentComponent(&LookupError) : Proposed.Get();
    if (LookupError.IsValid()) { SetFailure(OutFailure, MoveTemp(LookupError)); return false; }
    if (!BuildRequests(Expected,
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
    FDiagnostic LookupError;
    UActorComponent* Current = Operation == EOperation::Remove ? nullptr : ResolveCurrentComponent(&LookupError);
    if (LookupError.IsValid()) { SetFailure(OutFailure, MoveTemp(LookupError)); return false; }
    return BuildRequests(Current, Operation == EOperation::Remove, OutRequests, OutFailure);
  }
  virtual bool BuildDomainBeforeReadRequests(
      TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
      FOperationFailure& OutFailure) const override {
    FDiagnostic LookupError;
    UActorComponent* Owner = ResolveCurrentComponent(&LookupError);
    if (LookupError.IsValid()) { SetFailure(OutFailure, MoveTemp(LookupError)); return false; }
    return BuildRequests(Owner, !Owner, OutRequests, OutFailure);
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
      SetFailure(OutFailure, StatePresenceFailure(TEXT("level_world_not_found"),
          TEXT("The target world is no longer available for component rollback."), TEXT("world_present"), false, true));
      return false;
    }
    UE::UAI::Contract::FDiagnostic OwnerError;
    AActor *Owner = Persistent.IsValid() ? Persistent->FindActor(*World, OwnerTarget, OwnerError) : FindActorExact(*World, OwnerTarget, OwnerError);
    if (!Owner) {
      if (OwnerError.IsEmpty()) OwnerError = StatePresenceFailure(TEXT("level_component_owner_not_found"),
          TEXT("The component owner actor is no longer available for rollback."), TEXT("owner_actor_present"), false, true);
      if (OwnerError.SourceKind.IsEmpty()) OwnerError.SourceKind = TEXT("native_result");
      SetFailure(OutFailure, PublicLookupFailure(OwnerError, EAuthorabilityPublicTarget::Component));
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
      UE::UAI::Contract::FDiagnostic PcgError;
      if (Pcg.IsValid() && !UeAgentPcgComponentAuthoring::Apply(
              *Actual, UeAgentPcgComponentAuthoring::Read(*Backup), PcgError,
              ReadString(ReadObject(DiagnosticInput, TEXT("target")), TEXT("world_path")))) {
        if (PcgError.SourceKind.IsEmpty()) PcgError.SourceKind = TEXT("native_result");
        if (PcgError.CandidateName.IsEmpty()) PcgError.CandidateName = Identity.GetSelector(TEXT("component_name"));
        SetFailure(OutFailure, MoveTemp(PcgError));
        return false;
      }
      if (bHadSceneState) {
        FAttachmentPatch Restore;
        Restore.bPresent = true;
        Restore.ParentComponent = OriginalParentName;
        Restore.SocketName = OriginalSocketName;
        Restore.Transform =
            FTransformPatch{OriginalRelativeTransform.GetLocation(),
                            OriginalRelativeTransform.Rotator(),
                            OriginalRelativeTransform.GetScale3D()};
        UE::UAI::Contract::FDiagnostic RestoreError;
        if (!ApplyComponentAttachment(*Actual, *Owner, Restore, RestoreError)) {
          SetFailure(OutFailure, PublicLookupFailure(RestoreError, EAuthorabilityPublicTarget::Component));
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
        SetFailure(OutFailure, StatePresenceFailure(TEXT("level_component_remove_rollback_failed"),
            TEXT("Unreal could not recreate the removed component during rollback."), TEXT("restored_component_created"), false, true));
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
        UE::UAI::Contract::FDiagnostic RestoreError;
        if (!ApplyComponentAttachment(*Actual, *Owner, Restore, RestoreError)) {
          SetFailure(OutFailure, PublicLookupFailure(RestoreError, EAuthorabilityPublicTarget::Component));
          return false;
        }
      }
      if (bWasRootComponent) {
        USceneComponent *RestoredRoot = Cast<USceneComponent>(Actual);
        if (!RestoredRoot || !Owner->SetRootComponent(RestoredRoot)) {
          SetFailure(OutFailure, StatePresenceFailure(TEXT("level_component_remove_rollback_failed"),
              !RestoredRoot ? TEXT("The restored root component is not a SceneComponent.")
                  : TEXT("Unreal rejected restoring the owner's root component."),
              !RestoredRoot ? TEXT("restored_root_is_scene_component") : TEXT("root_component_restored"), false, true));
          return false;
        }
      }
      Existing.Reset(Actual);
    }
    return true;
  }

private:
  UActorComponent* ResolveCurrentComponent(FDiagnostic* OutError = nullptr) const {
    if (OutError) OutError->Reset();
    if (!World.IsValid()) return nullptr;
    FDiagnostic Error;
    AActor* Owner = Persistent.IsValid() ? Persistent->FindActor(*World, OwnerTarget, Error) : FindActorExact(*World, OwnerTarget, Error);
    UActorComponent* Component = Owner ? FindComponentExact(*Owner,
        Identity.GetSelector(TEXT("component_name")), Error) : nullptr;
    if (OutError && Error.IsValid()) *OutError = PublicLookupFailure(Error, EAuthorabilityPublicTarget::Component);
    return Component;
  }

  bool BuildRequests(UActorComponent *Component, const bool bExpectedAbsent,
                     TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
                     FOperationFailure &OutFailure) const {
    UE::UAI::Contract::FDiagnostic Error;
    if (!BuildComponentSnapshotReadRequests(
            Component, Identity, bExpectedAbsent, OutRequests, Error)) {
      SetFailure(OutFailure, MoveTemp(Error));
      return false;
    }
    OutFailure = {};
    return true;
  }

  void ClearPersistentReferences() override {
    World.Reset(); Existing.Reset(); Proposed.Reset(); Backup.Reset(); Actual = nullptr;
  }
  bool BindPersistentWorld(UWorld& Reloaded, bool bBefore, FOperationFailure& Failure) override {
    World = &Reloaded;
    UE::UAI::Contract::FDiagnostic Error;
    AActor* Owner = Persistent.IsValid() ? Persistent->FindActor(Reloaded, OwnerTarget, Error) : FindActorExact(Reloaded, OwnerTarget, Error);
    Actual = Owner ? FindComponentExact(*Owner, Identity.GetSelector(TEXT("component_name")), Error) : nullptr;
    Existing.Reset(Actual);
    if (Error.IsValid()) { SetFailure(Failure, PublicLookupFailure(Error, EAuthorabilityPublicTarget::Component)); return false; }
    const bool bAbsent = bBefore ? Operation == EOperation::Create : Operation == EOperation::Remove;
    if ((Actual == nullptr) != bAbsent) {
      SetFailure(Failure, Error.IsValid() ? MoveTemp(Error) : StatePresenceFailure(TEXT("level_component_persistent_rebind_failed"),
          bAbsent ? TEXT("The component is still present after reloading a state that requires its absence.")
              : TEXT("The component is missing after reloading a state that requires its presence."),
          TEXT("component_present"), Actual != nullptr, !bAbsent));
      return false;
    }
    return true;
  }

  TWeakObjectPtr<UWorld> World;
  TSharedRef<FLevelAuthoringScratchWorld> ScratchWorld;
  EOperation RequestedOperation = EOperation::Update;
  EOperation Operation = EOperation::Update;
  bool bAdoptMatchingComponentFromSpawnedOwner = false;
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
  TSharedPtr<const FJsonObject> Pcg;
};

class FLevelLifecycle final : public IReflectionOperationLifecycle {
public:
  FLevelLifecycle() = default;

  explicit FLevelLifecycle(UWorld &InBoundWorld, const TSharedPtr<FLevelPersistentContext>& InPersistent,
      bool bInSceneEditing)
      : BoundWorld(&InBoundWorld),
        SharedScratchWorld(MakeShared<FLevelAuthoringScratchWorld>()),
        bHasBoundWorld(true), bSceneEditing(bInSceneEditing) {
    Persistent = InPersistent;
    if (Persistent.IsValid()) Persistent->Scratch = SharedScratchWorld;
  }

  virtual bool
  PrepareLifecycle(const FPrepareRequest &Request,
                   TUniquePtr<IReflectionOperationLifecycleState> &OutState,
                   FOperationFailure &OutFailure) const override {
    OutState.Reset();
    OutFailure = {};
    ON_SCOPE_EXIT {
      if (OutFailure.Diagnostic.IsValid() && OutFailure.Diagnostic.TargetPath.IsEmpty()
          && Request.Input && Request.Input->Target.IsValid())
        Request.Input->Target->TryGetStringField(TEXT("world_path"), OutFailure.Diagnostic.TargetPath);
    };
    if (!Request.IsValid() || !Request.Input->Target.IsValid()) {
      SetFailure(OutFailure, PresenceFailure(TEXT("level_apply_request_invalid"),
          !Request.IsValid() ? TEXT("Level apply requires a valid prepared request and contract.")
              : TEXT("Level apply requires a target object."),
          !Request.IsValid() ? TEXT("request_valid") : TEXT("target_present"), false, true));
      return false;
    }
    const FString WorldPath =
        ReadString(Request.Input->Target, TEXT("world_path"));
    UWorld *World = ResolveWorld(WorldPath);
    if (!World) {
      SetFailure(OutFailure, LevelFailure(TEXT("level_world_not_found"),
          TEXT("The requested world is not currently loaded in the allowed Editor context."),
          TEXT("world_path"), Request.Input->Target->TryGetField(TEXT("world_path")),
          MakeShared<FJsonValueString>(TEXT("loaded Editor world")), Request.Input->Target.Get(), TEXT("world_path")));
      return false;
    }

    if (Request.Contract->ContractId == WorldInputContractId) {
      UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
      UE::UAI::Contract::FDiagnostic Error;
      if (Request.Input->RequestedOperation != EOperation::Update) {
        auto OperationError = FDiagnostic::Failure(TEXT("level_world_operation_invalid"),
            TEXT("World settings support only the update operation."), TEXT("level_apply"));
        OperationError.SemanticPointer = TEXT("/operation");
        OperationError.ActualValue = MakeShared<FJsonValueString>(OperationToString(Request.Input->RequestedOperation));
        OperationError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("update"));
        SetFailure(OutFailure, MoveTemp(OperationError));
        return false;
      }
      if (!BuildWorldIdentity(*World, Identity, Error)) {
        SetFailure(OutFailure, MoveTemp(Error));
        return false;
      }
      TSharedRef<FLevelAuthoringScratchWorld> ScratchWorld = GetScratchWorld();
      AWorldSettings* Proposed = nullptr;
      if (Request.Input->HasProperties()) {
        AWorldSettings* Settings = World->GetWorldSettings();
        Proposed = Settings ? Cast<AWorldSettings>(ScratchWorld->DuplicateActor(*Settings, Error)) : nullptr;
        if (!Proposed) {
          SetFailure(OutFailure, Error.IsValid() ? MoveTemp(Error) : PresenceFailure(TEXT("level_world_settings_prepare_failed"),
              !Settings ? TEXT("The world has no WorldSettings object to prepare.")
                  : TEXT("The scratch copy of WorldSettings has the wrong native type."),
              !Settings ? TEXT("world_settings_present") : TEXT("scratch_is_world_settings"), false, true, WorldPath));
          return false;
        }
      }
      OutState = MakeUnique<FWorldLifecycleState>(*World, ScratchWorld, Proposed, MoveTemp(Identity));
      static_cast<FLevelPersistentState*>(OutState.Get())->CaptureInputSources(Request);
      static_cast<FLevelPersistentState*>(OutState.Get())->SetPersistentContext(Persistent);
      if (!static_cast<FWorldLifecycleState*>(OutState.Get())->PrepareTopology(*World, Request.Input->ProviderExtension, OutFailure)) return false;
      return true;
    }

    if (Request.Contract->ContractId == ActorInputContractId) {
      if (!PrepareActor(*World, Request, OutState, OutFailure)) return false;
      static_cast<FLevelPersistentState*>(OutState.Get())->CaptureInputSources(Request);
      static_cast<FLevelPersistentState*>(OutState.Get())->SetPersistentContext(Persistent);
      return static_cast<FActorLifecycleState*>(OutState.Get())->PrepareLevel(Request.Input->ProviderExtension, OutFailure);
    }
    if (Request.Contract->ContractId == ComponentInputContractId) {
      if (!PrepareComponent(*World, Request, OutState, OutFailure)) return false;
      static_cast<FLevelPersistentState*>(OutState.Get())->CaptureInputSources(Request);
      static_cast<FLevelPersistentState*>(OutState.Get())->SetPersistentContext(Persistent);
      return true;
    }
    auto Error = PresenceFailure(TEXT("level_apply_contract_unsupported"),
        TEXT("The prepared contract is not a Level world, actor or component contract."), TEXT("level_contract_supported"), false, true, WorldPath);
    Error.InternalContext = Request.Contract->ContractId;
    SetFailure(OutFailure, MoveTemp(Error));
    return false;
  }

private:
  bool ValidateSceneEditingProperties(const FPrepareRequest& Request, const UObject* Template,
      bool bComponent, FOperationFailure& OutFailure) const {
    if (!bSceneEditing) return true;
    for (const auto& Value : Request.Input->Properties) {
      const TSharedPtr<FJsonObject> Property = Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
      FString Path;
      if (!Property || !Property->TryGetStringField(TEXT("property_path"), Path)) continue;
      if (IsSceneEditingProperty(Path, bComponent, Template ? Template->GetClass() : nullptr)) continue;
      auto Error = FDiagnostic::Failure(TEXT("level_scene_editing_property_unsupported"),
          TEXT("This property on the resolved native class is outside scene_editing; use a full llm_authoring workspace to edit it."), TEXT("level_profile"));
      Error.PropertyPath = Path;
      Error.ActualValue = MakeShared<FJsonValueString>(Path);
      UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Property, TEXT("property_path"));
      SetFailure(OutFailure, MoveTemp(Error));
      return false;
    }
    return true;
  }

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
    UE::UAI::Contract::FDiagnostic Error;
    UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
    if (!BuildActorIdentityFromTarget(Request.Input->Target, Identity, Error)) {
      UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Request.Input->Target);
      SetFailure(OutFailure, MoveTemp(Error));
      return false;
    }
    AActor *Existing = FindActorExact(World, Request.Input->Target, Error);
    if (!Error.IsEmpty()) {
      SetFailure(OutFailure, PublicLookupFailure(Error, EAuthorabilityPublicTarget::Actor));
      return false;
    }
    EOperation Operation = Request.Input->RequestedOperation;
    if (Operation == EOperation::Upsert)
      Operation = Existing ? EOperation::Update : EOperation::Create;
    if ((Operation == EOperation::Create || Operation == EOperation::Clone) &&
        Existing) {
      auto ExistingError = TargetPresenceFailure(TEXT("level_actor_already_exists"),
          TEXT("An actor already matches the target of this create or clone operation."), Request.Input->Target, TEXT("actor_present"), true, false);
      ExistingError.CandidateName = Existing->GetActorLabel();
      SetFailure(OutFailure, MoveTemp(ExistingError));
      return false;
    }
    if ((Operation == EOperation::Update || Operation == EOperation::Remove) &&
        !Existing) {
      SetFailure(OutFailure, TargetPresenceFailure(TEXT("level_actor_not_found"),
          TEXT("No actor matches the target of this update or remove operation."), Request.Input->Target, TEXT("actor_present"), false, true));
      return false;
    }

	    FPlaceableActorPatch PlaceableCandidate;
	    FString CandidateActorClass;
	    if (!ParsePlaceableActorPatch(Request.Input->ProviderExtension,
	                                  PlaceableCandidate, Error)) {
	      SetFailure(OutFailure, MoveTemp(Error));
	      return false;
	    }
	    if (PlaceableCandidate.bPresent) {
	      if (Operation == EOperation::Clone || Operation == EOperation::Remove) {
	        auto OperationError = FDiagnostic::Failure(TEXT("level_actor_candidate_operation_unsupported"),
            TEXT("A placement candidate cannot be used with clone or remove."), TEXT("level_apply"));
        OperationError.SemanticPointer = TEXT("/operation");
        OperationError.ActualValue = MakeShared<FJsonValueString>(OperationToString(Operation));
        OperationError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("create, update or upsert"));
        SetFailure(OutFailure, MoveTemp(OperationError));
	        return false;
	      }
	      UeAgentPaletteSearch::FCandidate ResolvedCandidate;
	      if (!UeAgentPaletteSearch::TryResolvePlaceableActorCandidate(
	              PlaceableCandidate.Name, PlaceableCandidate.Id,
	              PlaceableCandidate.Category, ResolvedCandidate, Error,
	              PlaceableCandidate.Select)) {
	        const auto CandidateInput = ReadObject(Request.Input->ProviderExtension, TEXT("candidate"));
        if (CandidateInput.IsValid()) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *CandidateInput);
        SetFailure(OutFailure, MoveTemp(Error));
	        return false;
	      }
	      const FString ResolvedClass =
	          ResolvedCandidate.AuthoringSelectors.FindRef(TEXT("actor_class"));
	      CandidateActorClass = ResolvedClass;
	      const FString AssertedClass =
	          ReadString(Request.Input->Target, TEXT("actor_class"));
	      if (ResolvedClass.IsEmpty() ||
	          (!AssertedClass.IsEmpty() && AssertedClass != ResolvedClass)) {
	        const auto CandidateInput = ReadObject(Request.Input->ProviderExtension, TEXT("candidate"));
        auto ClassError = LevelFailure(TEXT("level_actor_candidate_class_mismatch"),
            ResolvedClass.IsEmpty() ? TEXT("The resolved placement candidate has no actor class.")
                : TEXT("The target actor_class differs from the resolved placement candidate class."),
            TEXT("actor_class"), MakeShared<FJsonValueString>(AssertedClass),
            MakeShared<FJsonValueString>(ResolvedClass.IsEmpty() ? TEXT("candidate with an actor class") : ResolvedClass),
            ResolvedClass.IsEmpty() ? CandidateInput.Get() : Request.Input->Target.Get(),
            ResolvedClass.IsEmpty() ? FString() : TEXT("actor_class"));
        ClassError.CandidateName = PlaceableCandidate.Name;
        if (ResolvedClass.IsEmpty()) ClassError.ActualValue = MakeShared<FJsonValueString>(ResolvedClass);
        SetFailure(OutFailure, MoveTemp(ClassError));
	        return false;
	      }
	      if (!ValidatePlaceableActorAssertions(Request.Input->ProviderExtension,
	                                             ResolvedCandidate, Error)) {
	        SetFailure(OutFailure, MoveTemp(Error));
	        return false;
	      }
	      if (Operation == EOperation::Create &&
	          !ResolvedCandidate.bSupportedInContext) {
	        SetFailure(OutFailure, UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("level_actor_candidate_not_headless_creatable"), ResolvedCandidate.SupportDiagnostic));
	        if (OutFailure.Diagnostic.TargetPath.IsEmpty())
	          OutFailure.Diagnostic.TargetPath = ReadString(Request.Input->Target, TEXT("world_path"));
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
          SetFailure(OutFailure, ClassAuthorability.Diagnostic);
          return false;
        }
      }
      Template = Class ? Class->GetDefaultObject<AActor>() : nullptr;
      if (!Template) {
        const auto CandidateInput = ReadObject(Request.Input->ProviderExtension, TEXT("candidate"));
        SetFailure(OutFailure, LevelFailure(TEXT("level_actor_class_invalid"),
            Class ? TEXT("The actor class has no actor default object.") : TEXT("The requested actor class could not be loaded."),
            TEXT("actor_class"), MakeShared<FJsonValueString>(ClassPath),
            MakeShared<FJsonValueString>(TEXT("loadable Actor class with a default object")),
            CandidateActorClass.IsEmpty() ? Request.Input->Target.Get() : CandidateInput.Get(),
            CandidateActorClass.IsEmpty() ? TEXT("actor_class") : FString()));
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
      FDerivedTargetSources SourceScope(&SourceTarget.Get(), Request.Input->Target.Get(), {
          {TEXT("/world_path"), TEXT("world_path")}, {TEXT("/actor_guid"), TEXT("source_actor_guid")},
          {TEXT("/managed_id"), TEXT("source_managed_id")}});
      Template = FindActorExact(World, SourceTarget, Error);
      if (!Template) {
        if (Error.IsEmpty()) Error = LevelFailure(TEXT("level_actor_clone_source_not_found"),
            TEXT("No actor matches the requested clone source."), TEXT("source_actor_present"),
            MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true),
            Request.Input->Target.Get(), SourceGuid.IsEmpty() ? TEXT("source_managed_id") : TEXT("source_actor_guid"));
        SetFailure(OutFailure, PublicLookupFailure(Error, EAuthorabilityPublicTarget::Actor));
        return false;
      }
    } else if (Existing) {
      const FString ClassPath =
          ReadString(Request.Input->Target, TEXT("actor_class"));
      if (!ClassPath.IsEmpty() &&
          Existing->GetClass()->GetPathName() != ClassPath) {
        auto ClassError = LevelFailure(TEXT("level_actor_class_conflict"),
            TEXT("The target actor's class differs from the requested actor_class."),
            TEXT("actor_class"), MakeShared<FJsonValueString>(Existing->GetClass()->GetPathName()),
            MakeShared<FJsonValueString>(ClassPath), Request.Input->Target.Get(), TEXT("actor_class"));
        ClassError.CandidateName = Existing->GetActorLabel();
        SetFailure(OutFailure, MoveTemp(ClassError));
        return false;
      }
    }

    if (!ValidateSceneEditingProperties(Request, Template, false, OutFailure)) return false;
    const TSharedRef<FLevelAuthoringScratchWorld> ScratchWorld =
        GetScratchWorld();
    AActor *Proposed = Operation == EOperation::Remove
                           ? Existing
                           : ScratchWorld->DuplicateActor(*Template, Error);
    if (Operation != EOperation::Remove && !Proposed) {
      SetFailure(OutFailure, MoveTemp(Error));
      return false;
    }
    if (Proposed && Operation != EOperation::Remove) {
      ScratchWorld->RegisterPreparedActor(Identity.CanonicalKey, *Proposed);
    }
    TOptional<FString> Label;
    TOptional<FString> Folder;
    TOptional<FTransformPatch> Transform;
    TOptional<TArray<FName>> Tags;
    FActorAttachmentPatch Attachment;
    if (!ParseActorExtension(Request.Input->ProviderExtension, Label, Folder,
                             Transform, Tags, Error)) {
      SetFailure(OutFailure, MoveTemp(Error));
      return false;
    }
    if (Proposed && Operation != EOperation::Remove) ApplyActorTags(*Proposed, Tags);
    if (!ParseActorAttachmentExtension(Request.Input->ProviderExtension,
                                       Attachment, Error)) {
      SetFailure(OutFailure, MoveTemp(Error));
      return false;
    }
    // Exported context is not a request to restore a relation changed by an
    // earlier native mutation in this batch (for example deleting the parent).
    if (Operation == EOperation::Update && IsUnchangedActorAttachment(Existing, Attachment))
      Attachment.bPresent = false;
    // GetActorLabel can cache a generated label. Never mutate a class default
    // or archetype while reading the baseline used by a prepared operation.
    const FString BaseLabel = Template->GetActorLabel(!Template->IsTemplate());
    bool bPreserveDataLayerPreview = !Template->GetDataLayerAssets().IsEmpty();
    for (const TSharedPtr<FJsonValue>& Value : Request.Input->Properties) {
      const TSharedPtr<FJsonObject> Property = Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
      FString Path;
      if (Property.IsValid() && Property->TryGetStringField(TEXT("property_path"), Path)
          && (Path == TEXT("DataLayerAssets") || Path.StartsWith(TEXT("DataLayerAssets["))))
        bPreserveDataLayerPreview = true;
    }
    OutState = MakeUnique<FActorLifecycleState>(
        World, ScratchWorld, Operation, MoveTemp(Identity), Existing, Proposed,
        BaseLabel, GetActorFolderPathText(*Template),
        Template->GetActorTransform(), MoveTemp(Label), MoveTemp(Folder),
	        MoveTemp(Transform), MoveTemp(Tags), MoveTemp(Attachment),
	        MoveTemp(PlaceableCandidate), bPreserveDataLayerPreview);
    if (Persistent.IsValid() && Existing) Persistent->TrackLevel(Existing->GetLevel());
    return true;
  }

  bool
  PrepareComponent(UWorld &World, const FPrepareRequest &Request,
                   TUniquePtr<IReflectionOperationLifecycleState> &OutState,
                   FOperationFailure &OutFailure) const {
    UE::UAI::Contract::FDiagnostic Error;
    UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
    if (!BuildComponentIdentityFromTarget(Request.Input->Target, Identity,
                                          Error)) {
      UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Request.Input->Target);
      SetFailure(OutFailure, MoveTemp(Error));
      return false;
    }
    const TSharedRef<FJsonObject> OwnerTarget =
        MakeOwnerActorTarget(Request.Input->Target);
    FDerivedTargetSources OwnerSources(&OwnerTarget.Get(), Request.Input->Target.Get(), {
        {TEXT("/world_path"), TEXT("world_path")}, {TEXT("/actor_guid"), TEXT("owner_actor_guid")},
        {TEXT("/managed_id"), TEXT("owner_managed_id")}});
    UeAgentAuthoringIdentity::FCanonicalIdentity OwnerIdentity;
    if (!BuildActorIdentityFromTarget(OwnerTarget, OwnerIdentity, Error)) {
      UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, OwnerTarget.Get());
      SetFailure(OutFailure, MoveTemp(Error));
      return false;
    }
    AActor *Owner = FindActorExact(World, OwnerTarget, Error);
    if (Persistent.IsValid() && Owner) Persistent->TrackLevel(Owner->GetLevel());
    if (!Error.IsEmpty()) {
      SetFailure(OutFailure, PublicLookupFailure(Error, EAuthorabilityPublicTarget::Component));
      return false;
    }
    UActorComponent *Existing = nullptr;
    if (Owner) {
      Existing = FindComponentExact(
          *Owner, Identity.GetSelector(TEXT("component_name")), Error);
      if (!Error.IsEmpty()) {
        UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Request.Input->Target, TEXT("component_name"));
        SetFailure(OutFailure, PublicLookupFailure(Error, EAuthorabilityPublicTarget::Component));
        return false;
      }
    }

    const EOperation RequestedOperation = Request.Input->RequestedOperation;
    EOperation Operation = RequestedOperation;
    if (Operation == EOperation::Upsert)
      Operation = Existing ? EOperation::Update : EOperation::Create;
    const bool bAdoptMatchingComponentFromSpawnedOwner =
        Owner == nullptr &&
        (RequestedOperation == EOperation::Create ||
         RequestedOperation == EOperation::Upsert);
    if ((Operation == EOperation::Create || Operation == EOperation::Clone) &&
        Existing) {
      SetFailure(OutFailure, TargetPresenceFailure(TEXT("level_component_already_exists"),
          TEXT("The owner already has a component with the target name for this create or clone operation."),
          Request.Input->Target, TEXT("component_present"), true, false));
      return false;
    }
    if ((Operation == EOperation::Update || Operation == EOperation::Remove) &&
        !Existing && Owner) {
      SetFailure(OutFailure, TargetPresenceFailure(TEXT("level_component_not_found"),
          TEXT("No component matches the target of this update or remove operation."),
          Request.Input->Target, TEXT("component_present"), false, true));
      return false;
    }
    if (Operation == EOperation::Remove && !Owner) {
      SetFailure(OutFailure, TargetPresenceFailure(TEXT("level_component_owner_not_found"),
          TEXT("The owner actor of the component to remove was not found."),
          Request.Input->Target, TEXT("owner_actor_present"), false, true));
      return false;
    }
    if (Operation == EOperation::Remove && Existing &&
        Existing->CreationMethod != EComponentCreationMethod::Instance) {
      SetFailure(OutFailure, LevelFailure(TEXT("level_component_remove_class_owned"),
          TEXT("This component is owned by the native Actor class or Blueprint construction; remove it at that source, not from a Level instance."),
          TEXT("creation_method"), MakeShared<FJsonValueNumber>(static_cast<int32>(Existing->CreationMethod)),
          MakeShared<FJsonValueNumber>(static_cast<int32>(EComponentCreationMethod::Instance)),
          Request.Input->Target.Get(), TEXT("component_name")));
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
          SetFailure(OutFailure, ClassAuthorability.Diagnostic);
          return false;
        }
      }
      Template = Class ? Class->GetDefaultObject<UActorComponent>() : nullptr;
      if (!Template) {
        SetFailure(OutFailure, LevelFailure(TEXT("level_component_class_invalid"),
            Class ? TEXT("The component class has no component default object.") : TEXT("The requested component class could not be loaded."),
            TEXT("component_class"), MakeShared<FJsonValueString>(ClassPath),
            MakeShared<FJsonValueString>(TEXT("loadable ActorComponent class with a default object")),
            Request.Input->Target.Get(), TEXT("component_class")));
        return false;
      }
    } else if (Operation == EOperation::Clone) {
      if (!Owner) {
        SetFailure(OutFailure, TargetPresenceFailure(TEXT("level_component_clone_owner_not_found"),
            TEXT("The owner actor must already exist before cloning one of its components."),
            Request.Input->Target, TEXT("owner_actor_present"), false, true));
        return false;
      }
      Template = FindComponentExact(
          *Owner,
          ReadString(Request.Input->Target, TEXT("source_component_name")),
          Error);
      if (!Template) {
        if (Error.IsEmpty()) Error = LevelFailure(TEXT("level_component_clone_source_not_found"),
            TEXT("The owner has no component matching source_component_name."), TEXT("source_component_name"),
            Request.Input->Target->TryGetField(TEXT("source_component_name")), MakeShared<FJsonValueString>(TEXT("existing component name")),
            Request.Input->Target.Get(), TEXT("source_component_name"));
        else UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Request.Input->Target, TEXT("source_component_name"));
        SetFailure(OutFailure, PublicLookupFailure(Error, EAuthorabilityPublicTarget::Component));
        return false;
      }
    } else if (Existing) {
      const FString ClassPath =
          ReadString(Request.Input->Target, TEXT("component_class"));
      if (!ClassPath.IsEmpty() &&
          Existing->GetClass()->GetPathName() != ClassPath) {
        SetFailure(OutFailure, LevelFailure(TEXT("level_component_class_conflict"),
            TEXT("The existing component's class differs from the requested component_class."),
            TEXT("component_class"), MakeShared<FJsonValueString>(Existing->GetClass()->GetPathName()),
            MakeShared<FJsonValueString>(ClassPath), Request.Input->Target.Get(), TEXT("component_class")));
        return false;
      }
    }

    if (!ValidateSceneEditingProperties(Request, Template, true, OutFailure)) return false;
    const TSharedRef<FLevelAuthoringScratchWorld> ScratchWorld =
        GetScratchWorld();
    UActorComponent *Proposed = Existing;
    if (Operation != EOperation::Remove) {
      AActor *ScratchOwner =
          Owner ? ScratchWorld->DuplicateActor(*Owner, Error)
                : ScratchWorld->FindPreparedActor(OwnerIdentity.CanonicalKey);
      if (!ScratchOwner) {
        SetFailure(OutFailure, Error.IsValid() ? MoveTemp(Error) : TargetPresenceFailure(TEXT("level_component_scratch_owner_unresolved"),
            TEXT("The component owner has neither an existing actor nor a prepared actor in this workspace."),
            Request.Input->Target, TEXT("prepared_owner_present"), false, true));
        return false;
      }
      const bool bReuseMatchingComponent =
          Operation == EOperation::Update ||
          RequestedOperation == EOperation::Upsert;
      if (!ScratchOwner->IsA(Template->GetClass()->ClassWithin)) {
        SetFailure(OutFailure, LevelFailure(TEXT("level_component_owner_class_incompatible"),
            TEXT("The component class requires a different native outer class."), TEXT("component_class"),
            MakeShared<FJsonValueString>(ScratchOwner->GetClass()->GetPathName()),
            MakeShared<FJsonValueString>(Template->GetClass()->ClassWithin->GetPathName()),
            Request.Input->Target.Get(), TEXT("component_class")));
        return false;
      }
      Proposed = ScratchWorld->CreateComponent(
          *Template, *ScratchOwner,
          FName(*Identity.GetSelector(TEXT("component_name"))),
          bReuseMatchingComponent, Error);
    }
    if (Operation != EOperation::Remove && !Proposed) {
      SetFailure(OutFailure, MoveTemp(Error));
      return false;
    }
    const FString AssetOverride = ReadString(Request.Input->ProviderExtension, TEXT("asset_override"));
    if (Operation != EOperation::Remove && !AssetOverride.IsEmpty()) {
      UObject* Asset = LoadObject<UObject>(nullptr, *AssetOverride);
      if (!Asset || !FComponentAssetBrokerage::AssignAssetToComponent(Proposed, Asset)) {
        SetFailure(OutFailure, LevelFailure(TEXT("level_component_candidate_asset_unsupported"),
            TEXT("The native component asset broker could not assign the selected candidate asset."),
            TEXT("asset_override"), MakeShared<FJsonValueString>(AssetOverride),
            MakeShared<FJsonValueString>(TEXT("asset supported by the component's native broker")),
            Request.Input->ProviderExtension.Get(), TEXT("asset_override")));
        return false;
      }
    }
    FAttachmentPatch Attachment;
    if (!ParseAttachmentExtension(Request.Input->ProviderExtension, Attachment,
                                  Error)) {
      SetFailure(OutFailure, MoveTemp(Error));
      return false;
    }
    OutState = MakeUnique<FComponentLifecycleState>(
        World, ScratchWorld, RequestedOperation, Operation,
        bAdoptMatchingComponentFromSpawnedOwner, MoveTemp(Identity),
        OwnerTarget, Existing, Proposed,
        MoveTemp(Attachment), ReadObject(Request.Input->ProviderExtension, TEXT("pcg")));
    return true;
  }

  TWeakObjectPtr<UWorld> BoundWorld;
  TSharedPtr<FLevelAuthoringScratchWorld> SharedScratchWorld;
  TSharedPtr<FLevelPersistentContext> Persistent;
  bool bHasBoundWorld = false;
  bool bSceneEditing = false;
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
  Result.PropertyPayload.Mode = TEXT("reflection");
  Result.PropertyPayload.PropertyPathField = TEXT("property_path");
  Result.PropertyPayload.ValueField = TEXT("value");
  Result.PropertyPayload.OptionalAssertionFields = {
      TEXT("kind"),         TEXT("struct_path"),  TEXT("enum_path"),
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
  Result.SemanticDescriptorRefs = {WorldStateSemanticId, WorldPropertySemanticId};
  Result.InputFields = {
      Field(TEXT("world_path"), TEXT("identity"), TEXT("ue_asset_path"),
            {EOperation::Update}),
      Field(TEXT("operation"), TEXT("operation_control"),
            TEXT("apply_operation"), {EOperation::Update}),
      Field(TEXT("properties"), TEXT("intent"),
            TEXT("property_descriptor_list")),
      Field(TEXT("streaming_levels"), TEXT("provider_extension"), TEXT("object")),
      Field(TEXT("world_partition"), TEXT("provider_extension"), TEXT("object")),
      Field(TEXT("data_layers"), TEXT("provider_extension"), TEXT("object")),
  };
  FOperationDescriptor Update = OperationDescriptor(EOperation::Update);
  Update.IdentityOneOf = {{TEXT("world_path")}};
  Update.AcceptedSections = {TEXT("target"), TEXT("properties"), TEXT("provider_extension")};
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
      Field(TEXT("actor_class"), TEXT("type_assertion"), TEXT("ue_class_path")),
      Field(TEXT("source_actor_guid"), TEXT("selector"), TEXT("guid")),
      Field(TEXT("source_managed_id"), TEXT("selector"), TEXT("string")),
      Field(TEXT("operation"), TEXT("operation_control"),
            TEXT("apply_operation"), All),
      Field(TEXT("properties"), TEXT("intent"),
            TEXT("property_descriptor_list")),
      Field(TEXT("label"), TEXT("provider_extension"), TEXT("string")),
      Field(TEXT("folder_path"), TEXT("provider_extension"), TEXT("string")),
      Field(TEXT("level"), TEXT("provider_extension"), TEXT("ue_asset_path")),
      Field(TEXT("transform"), TEXT("provider_extension"), TEXT("transform")),
      Field(TEXT("tags"), TEXT("provider_extension"), TEXT("string_array")),
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
      Field(TEXT("pcg"), TEXT("provider_extension"), TEXT("object")),
      Field(TEXT("asset_override"), TEXT("provider_extension"), TEXT("ue_asset_path")),
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

bool BuildDependenciesAndValidate(
    UWorld &World, const UeAgentAuthoringWorkspaceRouter::FWorkspace &Workspace,
    UeAgentAuthoringWorkspaceDependencyGraph::FDependencyInputs &OutInputs,
    UE::UAI::Contract::FDiagnostic& OutError) {
  OutInputs = {};
  OutError.Reset();
  TMap<FString, FOperationInput> OperationInputs;
  for (const UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit &Unit :
       Workspace.Units) {
    FScopedSourceLocations Sources(Unit.Document.Get(), [&Unit](const FString& Pointer) {
      return Unit.ResolveDiagnosticSource(Pointer);
    });
    if (Unit.Action != UeAgentAuthoringWorkspaceRouter::EUnitAction::Apply ||
        (Unit.UnitId != TEXT("world") && Unit.UnitId != TEXT("actor") &&
         Unit.UnitId != TEXT("component"))) {
      continue;
    }
    FContract Contract;
    FOperationInput Input;
    FOperationFailure InputFailure;
    if (!TryResolveWorkspaceOperationInput(
            Unit, FRegistry::Get(), Contract, Input, InputFailure)) {
      OutError = InputFailure.Diagnostic;
      return false;
    }
    OperationInputs.Add(Unit.CanonicalUnitKey, MoveTemp(Input));
  }

  TMap<FString, const UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit *>
      ActorUnits;
  TMap<const AActor *, const UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit *>
      ResolvedActorUnits;
  TMap<FString, const UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit *>
      ComponentUnits;
  for (const UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit &Unit :
       Workspace.Units) {
    FScopedSourceLocations Sources(Unit.Document.Get(), [&Unit](const FString& Pointer) {
      return Unit.ResolveDiagnosticSource(Pointer);
    });
    if (Unit.UnitId == TEXT("world") &&
        Unit.Action == UeAgentAuthoringWorkspaceRouter::EUnitAction::Apply) {
      const FOperationInput &Input =
          OperationInputs.FindChecked(Unit.CanonicalUnitKey);
      if (ReadString(Input.Target, TEXT("world_path")) !=
          GetWorldPath(World)) {
        OutError = LevelFailure(TEXT("level_workspace_world_conflict"),
            TEXT("The workspace world_path differs from the world being imported into."),
            TEXT("world_path"), Input.Target.IsValid() ? Input.Target->TryGetField(TEXT("world_path")) : nullptr,
            MakeShared<FJsonValueString>(GetWorldPath(World)), Input.Target.Get(), TEXT("world_path"));
        OutError.TargetPath = GetWorldPath(World);
        return false;
      }
    } else if (Unit.UnitId == TEXT("actor") &&
               Unit.Action ==
                   UeAgentAuthoringWorkspaceRouter::EUnitAction::Apply) {
      const FOperationInput &Input =
          OperationInputs.FindChecked(Unit.CanonicalUnitKey);
      UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
      UE::UAI::Contract::FDiagnostic Error;
      if (!BuildActorIdentityFromTarget(Input.Target, Identity, Error)) {
        OutError = MoveTemp(Error);
        if (Input.Target.IsValid()) UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Input.Target);
        return false;
      }
      ActorUnits.Add(Identity.CanonicalKey, &Unit);
      // Actor units and component/attachment references may use different
      // selectors for the same live actor (GUID versus managed ID).
      // Preserve the declared identity for new actors; resolve existing actors
      // so their dependent mutations still run after the owner mutation.
      UE::UAI::Contract::FDiagnostic ResolveError;
      if (const AActor *Actor = FindActorExact(World, Input.Target, ResolveError)) {
        ResolvedActorUnits.Add(Actor, &Unit);
      }
      UeAgentAuthoringWorkspaceDependencyGraph::FDependencyDeclaration
          Dependency;
      Dependency.SourceUnitKey = Unit.CanonicalUnitKey;
      Dependency.TargetReference = TEXT("world");
      Dependency.SourceJsonPointer = TEXT("/target/world_path");
      OutInputs.ContractDependencies.Add(MoveTemp(Dependency));
    } else if (Unit.UnitId == TEXT("component") &&
               Unit.Action ==
                   UeAgentAuthoringWorkspaceRouter::EUnitAction::Apply) {
      const FOperationInput &Input =
          OperationInputs.FindChecked(Unit.CanonicalUnitKey);
      UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
      UE::UAI::Contract::FDiagnostic Error;
      if (!BuildComponentIdentityFromTarget(Input.Target, Identity, Error)) {
        OutError = MoveTemp(Error);
        if (Input.Target.IsValid()) UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Input.Target);
        return false;
      }
      ComponentUnits.Add(Identity.CanonicalKey, &Unit);
    }
  }

  for (const UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit &Unit :
       Workspace.Units) {
    FScopedSourceLocations Sources(Unit.Document.Get(), [&Unit](const FString& Pointer) {
      return Unit.ResolveDiagnosticSource(Pointer);
    });
    if (Unit.UnitId != TEXT("actor") ||
        Unit.Action != UeAgentAuthoringWorkspaceRouter::EUnitAction::Apply)
      continue;
    const FOperationInput &Input =
        OperationInputs.FindChecked(Unit.CanonicalUnitKey);
    if (Input.RequestedOperation == EOperation::Remove) continue;
    const TSharedPtr<const FJsonObject> &Extension = Input.ProviderExtension;
    const TSharedPtr<const FJsonObject> Attachment =
        ReadObject(Extension, TEXT("attachment"));
    const FString ParentManagedId =
        ReadString(Attachment, TEXT("parent_managed_id"));
    if (ParentManagedId.IsEmpty())
      continue;
    TSharedRef<FJsonObject> ParentTarget = MakeShared<FJsonObject>();
    ParentTarget->SetStringField(TEXT("world_path"),
                                 ReadString(Input.Target, TEXT("world_path")));
    ParentTarget->SetStringField(TEXT("managed_id"), ParentManagedId);
    FScopedSourceLocations ParentSources(&ParentTarget.Get(), [&Input, &Attachment](const FString& Pointer) {
      UE::UAI::Contract::FSourceLocation Source;
      if (Pointer == TEXT("/world_path") && Input.Target.IsValid())
        FScopedSourceLocations::Locate(*Input.Target, TEXT("world_path"), Source);
      else if (Attachment.IsValid())
        FScopedSourceLocations::Locate(*Attachment, TEXT("parent_managed_id"), Source);
      return Source;
    });
    UeAgentAuthoringIdentity::FCanonicalIdentity ParentIdentity;
    UE::UAI::Contract::FDiagnostic ParentError;
    if (!BuildActorIdentityFromTarget(ParentTarget, ParentIdentity,
                                      ParentError)) {
      OutError = MoveTemp(ParentError);
      UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, ParentTarget.Get());
      return false;
    }
    const UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit *const *ParentUnit =
        ActorUnits.Find(ParentIdentity.CanonicalKey);
    AActor *ExistingParent = FindActorExact(World, ParentTarget, ParentError);
    if (!ParentUnit && ExistingParent) {
      ParentUnit = ResolvedActorUnits.Find(ExistingParent);
    }
    if (!ParentUnit && !ExistingParent) {
      OutError = ParentError.IsValid() ? MoveTemp(ParentError) : LevelFailure(TEXT("level_actor_attachment_parent_not_found"),
          TEXT("The attachment parent is neither an existing actor nor an actor declared by this workspace."),
          TEXT("parent_actor_present"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true),
          Attachment.Get(), TEXT("parent_managed_id"));
      return false;
    }
    if (ParentUnit) {
      const FOperationInput &ParentInput =
          OperationInputs.FindChecked((*ParentUnit)->CanonicalUnitKey);
      if (ParentInput.RequestedOperation == EOperation::Remove) {
        FActorAttachmentPatch Patch;
        UE::UAI::Contract::FDiagnostic ActorError;
        const AActor* Actor = FindActorExact(World, Input.Target, ActorError);
        FDiagnostic AttachmentError;
        const bool bRetainedContext = (Input.RequestedOperation == EOperation::Update
            || Input.RequestedOperation == EOperation::Upsert)
            && ParseActorAttachmentExtension(Extension, Patch, AttachmentError)
            && IsUnchangedActorAttachment(Actor, Patch);
        if (!bRetainedContext) {
          OutError = LevelFailure(TEXT("level_actor_attachment_parent_removed"),
              TEXT("The requested attachment parent is removed by the same workspace."),
              TEXT("parent_operation"), MakeShared<FJsonValueString>(TEXT("remove")),
              MakeShared<FJsonValueString>(TEXT("parent remains present")), Attachment.Get(), TEXT("parent_managed_id"));
          if (ActorError.IsValid()) OutError.Append(ActorError);
          if (AttachmentError.IsValid()) OutError.Append(AttachmentError);
          return false;
        }
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
    FScopedSourceLocations Sources(Unit.Document.Get(), [&Unit](const FString& Pointer) {
      return Unit.ResolveDiagnosticSource(Pointer);
    });
    if (Unit.UnitId != TEXT("component") ||
        Unit.Action != UeAgentAuthoringWorkspaceRouter::EUnitAction::Apply)
      continue;
    const FOperationInput &Input =
        OperationInputs.FindChecked(Unit.CanonicalUnitKey);
    const EOperation ComponentOperation = Input.RequestedOperation;
    const TSharedRef<FJsonObject> OwnerTarget =
        MakeOwnerActorTarget(Input.Target);
    FDerivedTargetSources OwnerSources(&OwnerTarget.Get(), Input.Target.Get(), {
        {TEXT("/world_path"), TEXT("world_path")}, {TEXT("/actor_guid"), TEXT("owner_actor_guid")},
        {TEXT("/managed_id"), TEXT("owner_managed_id")}});
    UeAgentAuthoringIdentity::FCanonicalIdentity OwnerIdentity;
    UE::UAI::Contract::FDiagnostic Error;
    if (!BuildActorIdentityFromTarget(OwnerTarget, OwnerIdentity, Error)) {
      OutError = MoveTemp(Error);
      UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, OwnerTarget.Get());
      return false;
    }
    const UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit *const *OwnerUnit =
        ActorUnits.Find(OwnerIdentity.CanonicalKey);
    AActor *ExistingOwner = FindActorExact(World, OwnerTarget, Error);
    if (!OwnerUnit && ExistingOwner) {
      OwnerUnit = ResolvedActorUnits.Find(ExistingOwner);
    }
    if (!OwnerUnit && !ExistingOwner) {
      FAuthorabilityResult Authorability;
      if (TryMapAuthorabilityFailure(
              Error, EAuthorabilityPublicTarget::Component, Authorability)) {
        OutError = Authorability.Diagnostic;
      } else {
        OutError = Error.IsValid() ? MoveTemp(Error) : TargetPresenceFailure(TEXT("level_component_owner_unresolved"),
            TEXT("The component owner is neither an existing actor nor an actor declared by this workspace."),
            Input.Target, TEXT("owner_actor_present"), false, true);
      }
      return false;
    }
    if (OwnerUnit) {
      const FOperationInput &OwnerInput =
          OperationInputs.FindChecked((*OwnerUnit)->CanonicalUnitKey);
      const EOperation OwnerOperation = OwnerInput.RequestedOperation;
      if (OwnerOperation == EOperation::Remove &&
          ComponentOperation != EOperation::Remove) {
        OutError = LevelFailure(TEXT("level_component_owner_removed"),
            TEXT("The component cannot be applied because the same workspace removes its owner actor."),
            TEXT("owner_operation"), MakeShared<FJsonValueString>(OperationToString(OwnerOperation)),
            MakeShared<FJsonValueString>(TEXT("owner remains present unless the component is also removed")), Input.Target.Get());
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

    const TSharedPtr<const FJsonObject> &Extension = Input.ProviderExtension;
    const TSharedPtr<const FJsonObject> Attachment =
        ReadObject(Extension, TEXT("attachment"));
    const FString ParentName = ReadString(Attachment, TEXT("parent_component"));
    if (ParentName.IsEmpty())
      continue;
    UeAgentAuthoringIdentity::FCanonicalIdentity ParentIdentity;
    if (!UeAgentAuthoringIdentity::BuildCanonicalIdentity(
            ComponentIdentityModelId, TEXT("level_component"),
            {
                {TEXT("world_path"),
                 ReadString(Input.Target, TEXT("world_path"))},
                {TEXT("owner_actor_guid"),
                 ReadString(Input.Target, TEXT("owner_actor_guid"))},
                {TEXT("owner_managed_id"),
                 ReadString(Input.Target, TEXT("owner_managed_id"))},
                {TEXT("component_name"), ParentName},
            },
            ParentIdentity, Error)) {
      OutError = MoveTemp(Error);
      if (Attachment.IsValid()) UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Attachment, TEXT("parent_component"));
      return false;
    }
    const UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit *const *ParentUnit =
        ComponentUnits.Find(ParentIdentity.CanonicalKey);
    UE::UAI::Contract::FDiagnostic ParentError;
    UActorComponent *ExistingParent =
        ExistingOwner
            ? FindComponentExact(*ExistingOwner, ParentName, ParentError)
            : nullptr;
    bool bNativeDefaultParent = false;
    if (!ParentUnit && !ExistingParent && OwnerUnit) {
      const FOperationInput &OwnerInput =
          OperationInputs.FindChecked((*OwnerUnit)->CanonicalUnitKey);
      const FString OwnerClassPath =
          ReadString(OwnerInput.Target, TEXT("actor_class"));
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
      if (TryMapAuthorabilityFailure(
              ParentError, EAuthorabilityPublicTarget::Component,
              Authorability)) {
        OutError = Authorability.Diagnostic;
      } else {
        OutError = ParentError.IsValid() ? MoveTemp(ParentError) : LevelFailure(TEXT("level_component_parent_unresolved"),
            TEXT("The attachment parent is not an existing component, a workspace component or a native default component."),
            TEXT("parent_component"), MakeShared<FJsonValueString>(ParentName),
            MakeShared<FJsonValueString>(TEXT("available parent component name")), Attachment.Get(), TEXT("parent_component"));
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

TSharedRef<IApplyProvider> MakeBoundApplyProvider(UWorld &World, const TSharedPtr<FLevelPersistentContext>& Persistent,
    bool bSceneEditing) {
  const TSharedRef<IReflectionOperationLifecycle> Lifecycle =
      MakeShared<FLevelLifecycle>(World, Persistent, bSceneEditing);
  UeAgentAuthoringObjectApply::FOptions Options;
  Options.bCompactValues = true;
  Options.bAllowDomainAuthorizedOwners = true;
  Options.bAllowTemplateOwners = true;
  return MakeShared<FReflectionApplyProvider>(ApplyProviderId, Lifecycle,
                                              Options);
}
} // namespace

bool ResolvePlacementSelectors(const TSharedPtr<const FJsonObject>& Extension,
    TMap<FString, FString>& OutSelectors, FDiagnostic& OutError) {
  FPlaceableActorPatch Patch;
  if (!ParsePlaceableActorPatch(Extension, Patch, OutError)) return false;
  OutSelectors.Reset();
  if (!Patch.bPresent) return true;
  UeAgentPaletteSearch::FCandidate Candidate;
  if (!UeAgentPaletteSearch::TryResolvePlaceableActorCandidate(Patch.Name, Patch.Id,
      Patch.Category, Candidate, OutError, Patch.Select)
      || !ValidatePlaceableActorAssertions(Extension, Candidate, OutError)) return false;
  OutSelectors = Candidate.AuthoringSelectors;
  return true;
}

TSharedRef<FJsonObject> FWorkspaceApplyResult::ToJsonObject() const {
  TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
  Result->SetStringField(TEXT("status"), Status);
  Result->SetStringField(TEXT("phase"), Phase);
  if (!Diagnostic.IsEmpty())
  {
    Result->SetStringField(TEXT("error_code"), Diagnostic.Code);
    Result->SetObjectField(TEXT("diagnostic"), Diagnostic.ToJson());
  }
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

bool RegisterApplyContracts(UE::UAI::Contract::FDiagnostic& OutError) {
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
  Options.bCompactValues = true;
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
    OutResult.Diagnostic = PresenceFailure(TEXT("level_workspace_contract_missing"),
        TEXT("The Level workspace import contract is not registered."), TEXT("workspace_contract_registered"), false, true, GetWorldPath(World));
    OutResult.Diagnostic.Stage = OutResult.Phase;
    return false;
  }

  UeAgentAuthoringWorkspaceLoader::FLoadResult LoadResult;
  if (!UeAgentAuthoringWorkspaceLoader::LoadWorkspace(WorkspaceContract, Source,
                                                      LoadResult)) {
    OutResult.Phase = TEXT("load");
    OutResult.Diagnostic = LoadResult.Diagnostic;
    return false;
  }

  UeAgentAuthoringWorkspaceDependencyGraph::FDependencyInputs DependencyInputs;
  if (!BuildDependenciesAndValidate(World, LoadResult.Workspace,
                                    DependencyInputs, OutResult.Diagnostic)) {
    OutResult.Phase = TEXT("preflight");
    if (OutResult.Diagnostic.TargetPath.IsEmpty()) OutResult.Diagnostic.TargetPath = GetWorldPath(World);
    return false;
  }
  UeAgentAuthoringWorkspaceDependencyGraph::FDependencyGraph Graph;
  UeAgentAuthoringWorkspaceDependencyGraph::FGraphFailure GraphFailure;
  if (!UeAgentAuthoringWorkspaceDependencyGraph::BuildDependencyGraph(
          WorkspaceContract, LoadResult.Workspace, DependencyInputs, Graph,
          GraphFailure)) {
    OutResult.Phase = TEXT("preflight");
    OutResult.Diagnostic = GraphFailure.Diagnostic;
    return false;
  }

  UeAgentAuthoringOperationApply::FProviderRegistry ScopedProviders;
  UeAgentAuthoringOperationApply::ERegistrationResult ProviderRegistration;
  UE::UAI::Contract::FDiagnostic ProviderRegistrationError;
  TSharedPtr<FLevelPersistentContext> Persistent;
  if (Options.bPersistentReadback) Persistent = MakeShared<FLevelPersistentContext>(World);
  if (!ScopedProviders.RegisterProvider(MakeBoundApplyProvider(World, Persistent, Options.AuthoringProfileId == SceneEditingProfileId),
                                        ProviderRegistration,
                                        ProviderRegistrationError)) {
    OutResult.Phase = TEXT("registration");
    OutResult.Diagnostic = ProviderRegistrationError;
    return false;
  }

  if (Options.bDryRun) {
    UeAgentAuthoringOperationApply::FWorkspaceOperationPreflightProvider
        Provider(UeAgentApplyInputContract::FRegistry::Get(), ScopedProviders);
    UeAgentAuthoringWorkspaceTransaction::FPreflightResult Preflight;
    if (!UeAgentAuthoringWorkspaceTransaction::PreflightWorkspace(
            WorkspaceContract, LoadResult.Workspace, Graph, Provider,
            Preflight)) {
      const FApplyFailureProjection Failure = ProjectApplyFailure(Preflight);
      OutResult.Phase = Failure.Phase;
      OutResult.Diagnostic = Failure.Diagnostic;
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
    OutResult.Diagnostic = RequestFailure.Diagnostic;
    return false;
  }
  UeAgentAuthoringOperationApply::FBackendOptions BackendOptions;
  BackendOptions.bUseEditorTransaction = Options.bUseEditorTransaction;
  BackendOptions.FinalizeNativeUpdate = [&World, Persistent](FOperationFailure& Failure) {
    Failure = {};
    // Actor/component and brush updates enqueue navigation work. This import
    // saves and reloads without returning to the Editor tick, so process that
    // work once after all native input updates. In particular, newly generated
    // NavData and the tile updates dispatched by this work must be included in
    // the save. Otherwise reload can recreate an undersized mesh, and PIE can
    // copy incomplete static navigation before pausing the editor's builder.
    // Do not advance the world or force RebuildAll; retain the engine's build
    // policy and finish only work already queued on registered navigation data.
    if (UNavigationSystemBase* Navigation = World.GetNavigationSystem()) {
      if (auto* NativeNavigation = Cast<UNavigationSystemV1>(Navigation); NativeNavigation && Persistent.IsValid()) {
        Persistent->PublishNavigationBounds(*NativeNavigation);
      }
      Navigation->Tick(0.0f);
      for (TActorIterator<ANavigationData> It(&World); It; ++It) {
        ANavigationData* NavData = *It;
        if (IsValid(NavData) && NavData->IsRegistered()) {
          NavData->EnsureBuildCompletion();
        }
      }
    }
    return true;
  };
  if (Options.PackageStager.IsValid())
    BackendOptions.PackageStager = Options.PackageStager;
  ExecutionOptions.bSaveWhenCallerControlled = Options.bPersistentReadback;
  if (Options.bPersistentReadback) {
    BackendOptions.ObservePublishedReadback = [&World](
        const TArray<FPublishedPackageReadback>& Packages,
        TArray<FOperationReadback>& Readbacks,
        TFunctionRef<bool(FOperationFailure&)> Verify, FOperationFailure& Failure) -> TOptional<bool> {
      // This boundary is deliberately object/property based. Unknown native
      // types, structural edits, struct roots, and multi-package closures keep
      // the established unload/rebind implementation.
      if (World.IsPartitionedWorld() || Packages.Num() != 1 || Readbacks.IsEmpty()
          || Packages[0].Intent != EPackageDurableIntent::Save
          || Packages[0].PackageName != World.GetOutermost()->GetName()) return {};
      for (const FOperationReadback& Readback : Readbacks) {
        if (Readback.Operation != EOperation::Update) return {};
        bool bHasProperty = false;
        for (const auto& Request : Readback.Requests) {
          const UObject* Object = Request.RootObject;
          if (!Object || Object->GetWorld() != &World || Request.RootStruct || Request.RootData) return {};
          if (Request.SemanticId != TEXT("core.property")) continue;
          bHasProperty = true;
          const UClass* Class = Object->GetClass();
          const bool bLight = Class == UDirectionalLightComponent::StaticClass()
              || Class == UPointLightComponent::StaticClass() || Class == USpotLightComponent::StaticClass()
              || Class == URectLightComponent::StaticClass() || Class == USkyLightComponent::StaticClass();
          const bool bLightProperty = bLight && (Request.PropertyPath == TEXT("Intensity")
              || Request.PropertyPath == TEXT("AttenuationRadius"));
          const bool bPostProcessProperty = Class == APostProcessVolume::StaticClass()
              && (Request.PropertyPath == TEXT("Settings") || Request.PropertyPath.StartsWith(TEXT("Settings.")));
          if (!bLightProperty && !bPostProcessProperty) return {};
        }
        // Semantic Level bundles include the world topology context even
        // when only a component/actor property changed. Its actual topology
        // is still verified in the isolated world; it is not an omitted read.
        const bool bWorldTopology = Readback.Requests.Num() == 1
            && Readback.Requests[0].RootObject == &World
            && Readback.Requests[0].SemanticId == WorldStateSemanticId
            && World.GetStreamingLevels().IsEmpty();
        if (!bHasProperty && !bWorldTopology) return {};
      }
      const double Started = FPlatformTime::Seconds();
      TArray<UE::UAI::AssetDurable::FPersistedWorldPackage> Headers;
      Headers.Add({Packages[0].PackageName, Packages[0].FinalPath, false});
      const bool bObserved = UE::UAI::AssetDurable::TryObservePersistedWorld(Headers,
          [&Readbacks, &Verify, &World](UWorld& SnapshotWorld,
              const UE::UAI::AssetDurable::FPersistedWorldReferenceMap& References,
              FDiagnostic& Error) {
            ON_SCOPE_EXIT {
              // No raw snapshot object/struct pointer may survive its loader.
              for (auto& Readback : Readbacks) for (auto& Request : Readback.Requests) {
                Request.RootObject = nullptr; Request.NotificationOwner = nullptr;
              }
            };
            for (auto& Readback : Readbacks) for (auto& Request : Readback.Requests) {
              FString SnapshotPath;
              const UObject* Original = Request.RootObject;
              if (!References.TryMapReference(Original->GetPathName(), SnapshotPath)) {
                Error = PresenceFailure(TEXT("level_persisted_property_mapping_failed"),
                    TEXT("The property owner is outside the saved world reference map."),
                    TEXT("mapped_owner"), false, true, Original->GetPathName());
                return false;
              }
              UObject* SnapshotObject = StaticFindObject(UObject::StaticClass(), nullptr, *SnapshotPath);
              if (!SnapshotObject || SnapshotObject->GetClass() != Original->GetClass()) {
                Error = PresenceFailure(TEXT("level_persisted_property_owner_missing"),
                    TEXT("The independently loaded package did not contain the property owner with its original class."),
                    TEXT("owner_present"), SnapshotObject != nullptr, true, Original->GetPathName());
                return false;
              }
              if (const AActor* Actor = Cast<AActor>(SnapshotObject)) {
                const AActor* OriginalActor = Cast<AActor>(Original);
                // Normalize only the proven package namespace substitution;
                // labels, transforms, attachment and values remain actual reads.
                if (!Actor->GetLevel() || !OriginalActor->GetLevel()
                    || Actor->GetLevel()->GetPackage() != SnapshotWorld.GetOutermost()
                    || OriginalActor->GetLevel()->GetPackage() != World.GetOutermost()) {
                  Error = PresenceFailure(TEXT("level_persisted_property_level_mismatch"),
                      TEXT("The loaded actor's level is outside the saved map correspondence."),
                      TEXT("mapped_level"), false, true, Original->GetPathName());
                  return false;
                }
                Request.Context.Add(TEXT("actor_level_override"), World.GetOutermost()->GetName());
              }
              if (Request.SemanticId == TEXT("core.property")) {
                // Native accessor identity follows the actual snapshot object;
                // only the captured entry identity retains the source binding.
                // Setting RootObject alone leaves a stale accessor identity.
                UeAgentAuthoringState::FReadRequest Mapped;
                UeAgentAuthoringTargetLocator::FResolveResult Resolved;
                if (!UeAgentAuthoringState::BuildPropertyReadRequest(SnapshotObject,
                    Request.PropertyPath, Request.SemanticId, Mapped, Resolved)) {
                  Error = MoveTemp(Resolved.Diagnostic); return false;
                }
                Mapped.SnapshotIdentityOverride = Request.SnapshotIdentityOverride.IsSet()
                    ? Request.SnapshotIdentityOverride.GetValue() : Request.TargetIdentity;
                Mapped.Limits = Request.Limits;
                Mapped.Context = MoveTemp(Request.Context);
                Request = MoveTemp(Mapped);
              } else {
                Request.RootObject = SnapshotObject;
                if (Request.NotificationOwner) Request.NotificationOwner = SnapshotObject;
              }
            }
            FOperationFailure VerifyFailure;
            if (Verify(VerifyFailure)) return true;
            Error = MoveTemp(VerifyFailure.Diagnostic);
            return false;
          }, Failure.Diagnostic);
      UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=isolated_persisted_readback target=%s operations=%d total_s=%.6f success=%s"),
          *World.GetPathName(), Readbacks.Num(), FPlatformTime::Seconds() - Started,
          bObserved ? TEXT("true") : TEXT("false"));
      if (bObserved) ActivateLevelCaptureReuse(World);
      return bObserved;
    };
    BackendOptions.FinalReadback = [&Options, Persistent](FOperationFailure& Failure) {
      if (Options.FinalReadback && !Options.FinalReadback(Failure)) return false;
      if (Options.RestoreEditorContext) {
        Persistent->ReleaseReadbackBindings();
        return Options.RestoreEditorContext(Failure);
      }
      return true;
    };
  }
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
    DiscardLevelCaptureReuse();
    FApplyFailureProjection Failure;
    TryProjectApplyFailure(ApplyResult, Failure);
    OutResult.Phase = Failure.Phase;
    OutResult.Diagnostic = Failure.Diagnostic;
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

} // namespace UeAgentLevelAuthoring
