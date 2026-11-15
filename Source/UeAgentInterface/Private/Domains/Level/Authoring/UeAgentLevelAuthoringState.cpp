// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Level/Authoring/UeAgentLevelAuthoringState.h"

#include "Components/ActorComponent.h"
#include "Components/LightComponentBase.h"
#include "Components/LocalLightComponent.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringAuthorability.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringDescriptors.h"
#include "Domains/Level/Authoring/UeAgentLevelAuthoringSpatial.h"
#include "Domains/Level/Authoring/UeAgentLevelWorldTopology.h"
#include "Domains/PCG/Authoring/UeAgentPcgComponentAuthoring.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/PostProcessVolume.h"
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
#include "WorldPartition/HLOD/HLODActor.h"

namespace UeAgentLevelAuthoring {
struct FLevelSemanticFacts {
  FString SemanticId;
  UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
  TMap<FString, FString> Context;
  FString ClassPath, Level, Label, Folder, Parent, Socket;
  TArray<FString> Tags;
  FTransform Transform = FTransform::Identity;
  bool bScene = false;
  bool bOmitLabel = false, bOmitTranslation = false, bOmitRotation = false, bOmitScale = false;
};
struct FLevelSemanticFactsSet {
  TMap<FString, TSharedPtr<const FLevelSemanticFacts>> Values;
  uint64 AllocatedBytes = 0;
  FString ActorDefinitionKey, ComponentDefinitionKey;
  // The existing semantic-fact producer already builds these two static
  // contracts. Keep that result until the last capture/composition consumer.
  TSharedPtr<FJsonValue> ActorDefinition, ComponentDefinition;
  TSharedPtr<FJsonObject> ObjectDeclaration;
  int64 TypeDeclarationReads = 0, SpatialDefinitionReads = 0;
};
namespace {
thread_local FLevelSemanticFactsSet* ActiveSemanticFacts = nullptr;
FLevelSemanticFacts ReadActorFacts(const AActor& Actor, const TMap<FString, FString>* Context);
FLevelSemanticFacts ReadComponentFacts(const UActorComponent& Component, const TMap<FString, FString>* Context);

FString SemanticFactsKey(const UeAgentAuthoringState::FReadRequest& Request) {
  const auto& Identity = Request.SnapshotIdentityOverride.IsSet()
      ? Request.SnapshotIdentityOverride.GetValue() : Request.TargetIdentity;
  return FString::FromInt(Request.SemanticId.Len()) + TEXT(":") + Request.SemanticId + Identity.CanonicalKey;
}

uint64 SemanticFactsSize(const FLevelSemanticFacts& Facts) {
  uint64 Size = sizeof(FLevelSemanticFacts) + Facts.SemanticId.GetAllocatedSize()
      + Facts.Identity.ModelId.GetAllocatedSize() + Facts.Identity.TargetKind.GetAllocatedSize()
      + Facts.Identity.KeyPattern.GetAllocatedSize() + Facts.Identity.CanonicalKey.GetAllocatedSize()
      + Facts.Identity.Selectors.GetAllocatedSize() + Facts.Context.GetAllocatedSize()
      + Facts.ClassPath.GetAllocatedSize() + Facts.Level.GetAllocatedSize() + Facts.Label.GetAllocatedSize()
      + Facts.Folder.GetAllocatedSize() + Facts.Parent.GetAllocatedSize() + Facts.Socket.GetAllocatedSize()
      + Facts.Tags.GetAllocatedSize();
  for (const auto& Pair : Facts.Identity.Selectors) Size += Pair.Key.GetAllocatedSize() + Pair.Value.GetAllocatedSize();
  for (const auto& Pair : Facts.Context) Size += Pair.Key.GetAllocatedSize() + Pair.Value.GetAllocatedSize();
  for (const auto& Tag : Facts.Tags) Size += Tag.GetAllocatedSize();
  return Size;
}

void RememberSemanticFacts(const UeAgentAuthoringState::FReadRequest& Request,
    TSharedPtr<const FLevelSemanticFacts> Facts) {
  if (!ActiveSemanticFacts) return;
  const FString Key = SemanticFactsKey(Request);
  if (const auto* Existing = ActiveSemanticFacts->Values.Find(Key))
    ActiveSemanticFacts->AllocatedBytes -= SemanticFactsSize(**Existing) + Key.GetAllocatedSize();
  ActiveSemanticFacts->Values.Remove(Key);
  const uint64 Bytes = SemanticFactsSize(*Facts) + Key.GetAllocatedSize();
  if (ActiveSemanticFacts->Values.Num() >= 20000
      || ActiveSemanticFacts->AllocatedBytes + Bytes + ActiveSemanticFacts->Values.GetAllocatedSize() > 16ull * 1024 * 1024) return;
  ActiveSemanticFacts->Values.Add(Key, MoveTemp(Facts));
  ActiveSemanticFacts->AllocatedBytes += Bytes;
}

TSharedPtr<const FLevelSemanticFacts> ObserveSemanticFacts(const UeAgentAuthoringState::FReadRequest& Request) {
  const UObject* Root = Request.RootObject;
  if (!IsValid(Root) || Root->IsTemplate()
      || Root->GetClass()->GetOutermost()->GetName() != TEXT("/Script/Engine")
      || !Root->GetClass()->HasAnyClassFlags(CLASS_Native)
      || Root->GetClass()->HasAnyClassFlags(CLASS_CompiledFromBlueprint)) return {};
  // Overrides and unknown context keep the existing accessor path. Do not infer
  // their output from a similar-looking native transform or target identity.
  for (const auto& Pair : Request.Context)
    if (Pair.Key != TEXT("scene_editing")) return {};
  const bool bActor = Request.SemanticId == ActorTargetSemanticId || Request.SemanticId == ActorStateSemanticId;
  const bool bComponent = Request.SemanticId == ComponentTargetSemanticId || Request.SemanticId == ComponentStateSemanticId;
  if (!bActor && !bComponent) return {};
  const AActor* Actor = bActor ? Cast<AActor>(Root) : nullptr;
  const UActorComponent* Component = bComponent ? Cast<UActorComponent>(Root) : nullptr;
  if ((!Actor && !Component) || (Component && Component->IsA<UPCGComponent>())) return {};
  auto Facts = MakeShared<FLevelSemanticFacts>();
  if (Request.SemanticId == ActorStateSemanticId) {
    *Facts = ReadActorFacts(*Actor, &Request.Context);
  } else if (Request.SemanticId == ComponentStateSemanticId) {
    *Facts = ReadComponentFacts(*Component, &Request.Context);
  }
  Facts->SemanticId = Request.SemanticId;
  Facts->Identity = Request.TargetIdentity;
  Facts->Context = Request.Context;
  Facts->ClassPath = Root->GetClass()->GetPathName();
  return Facts;
}

bool SameSemanticFacts(const FLevelSemanticFacts& A, const FLevelSemanticFacts& B) {
  const auto& X = A.Identity;
  const auto& Y = B.Identity;
  // These facts prove identical exported text, not native FName identity.
  // FString's default equality would hide case-only labels or selector text.
  const auto SameText = [](const FString& Left, const FString& Right) {
    return Left.Equals(Right, ESearchCase::CaseSensitive);
  };
  const auto SameTextMap = [&SameText](const TMap<FString, FString>& Left, const TMap<FString, FString>& Right) {
    if (Left.Num() != Right.Num()) return false;
    for (const auto& Pair : Left) {
      const auto Id = Right.FindId(Pair.Key);
      if (!Id.IsValidId()) return false;
      const auto& Other = Right.Get(Id);
      if (!SameText(Pair.Key, Other.Key) || !SameText(Pair.Value, Other.Value)) return false;
    }
    return true;
  };
  const auto SameTags = [&SameText](const TArray<FString>& Left, const TArray<FString>& Right) {
    if (Left.Num() != Right.Num()) return false;
    for (int32 Index = 0; Index < Left.Num(); ++Index)
      if (!SameText(Left[Index], Right[Index])) return false;
    return true;
  };
  const auto ExactScalar = [](double Left, double Right) { return FMemory::Memcmp(&Left, &Right, sizeof(double)) == 0; };
  const auto ExactVector = [&ExactScalar](const FVector& Left, const FVector& Right) {
    return ExactScalar(Left.X, Right.X) && ExactScalar(Left.Y, Right.Y) && ExactScalar(Left.Z, Right.Z);
  };
  const FQuat Q = A.Transform.GetRotation(), R = B.Transform.GetRotation();
  return SameText(A.SemanticId, B.SemanticId) && SameText(X.ModelId, Y.ModelId) && SameText(X.TargetKind, Y.TargetKind)
      && SameText(X.KeyPattern, Y.KeyPattern) && SameText(X.CanonicalKey, Y.CanonicalKey)
      && SameTextMap(X.Selectors, Y.Selectors) && SameTextMap(A.Context, B.Context)
      && SameText(A.ClassPath, B.ClassPath) && SameText(A.Level, B.Level) && SameText(A.Label, B.Label) && SameText(A.Folder, B.Folder)
      && SameText(A.Parent, B.Parent) && SameText(A.Socket, B.Socket) && SameTags(A.Tags, B.Tags) && A.bScene == B.bScene
      && A.bOmitLabel == B.bOmitLabel && A.bOmitTranslation == B.bOmitTranslation
      && A.bOmitRotation == B.bOmitRotation && A.bOmitScale == B.bOmitScale
      && ExactVector(A.Transform.GetTranslation(), B.Transform.GetTranslation())
      && ExactVector(A.Transform.GetScale3D(), B.Transform.GetScale3D())
      && ExactScalar(Q.X, R.X) && ExactScalar(Q.Y, R.Y) && ExactScalar(Q.Z, R.Z) && ExactScalar(Q.W, R.W);
}

TSharedRef<FJsonObject> BuildSemanticFactsValue(const FLevelSemanticFacts& Facts) {
  const auto Result = MakeShared<FJsonObject>();
  if (Facts.SemanticId == ActorTargetSemanticId || Facts.SemanticId == ComponentTargetSemanticId) {
    UeAgentAuthoringIdentity::WriteSelectorsFlat(Facts.Identity, Result);
    Result->SetStringField(Facts.SemanticId == ActorTargetSemanticId ? TEXT("actor_class") : TEXT("component_class"), Facts.ClassPath);
  } else {
    const bool bActor = Facts.SemanticId == ActorStateSemanticId;
    if (bActor) {
      Result->SetStringField(TEXT("level"), Facts.Level);
      if (!Facts.bOmitLabel) Result->SetStringField(TEXT("label"), Facts.Label);
      Result->SetStringField(TEXT("folder_path"), Facts.Folder);
      const auto Transform = BuildTransformState(Facts.Transform, ETransformFrame::ActorWorld);
      if (Facts.bOmitTranslation) Transform->RemoveField(TEXT("translation"));
      if (Facts.bOmitRotation) Transform->RemoveField(TEXT("rotation"));
      if (Facts.bOmitScale) Transform->RemoveField(TEXT("scale"));
      Result->SetObjectField(TEXT("transform"), Transform);
      TArray<TSharedPtr<FJsonValue>> Tags;
      for (const FString& Tag : Facts.Tags) Tags.Add(MakeShared<FJsonValueString>(Tag));
      Result->SetArrayField(TEXT("tags"), MoveTemp(Tags));
    }
    if (bActor || Facts.bScene) {
      const auto Attachment = MakeShared<FJsonObject>();
      Attachment->SetStringField(bActor ? TEXT("parent_managed_id") : TEXT("parent_component"), Facts.Parent);
      Attachment->SetStringField(TEXT("socket_name"), Facts.Socket);
      if (!bActor) Attachment->SetObjectField(TEXT("transform"), BuildTransformState(Facts.Transform, ETransformFrame::ComponentParentRelative));
      Result->SetObjectField(TEXT("attachment"), Attachment);
    }
  }
  return Result;
}

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

template <typename TRequest>
bool AddReadRequest(
    const FString &SemanticId,
    const UeAgentAuthoringIdentity::FCanonicalIdentity &Identity,
    const UObject *RootObject, const bool bExpectedAbsent,
    TArray<TRequest> &OutRequests, bool bSceneEditing = false) {
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
  if (bSceneEditing) Request.Context.Add(TEXT("scene_editing"), TEXT("true"));
  OutRequests.Add(MoveTemp(Request));
  return true;
}

template <typename TRequest>
bool AppendEditableProperties(
    const UObject *Object, const FString &SemanticId, const FString &ModelId,
    const FString &TargetKind, const TMap<FString, FString> &BaseSelectors,
    const TSet<FName> &Excluded,
    TArray<TRequest> &OutRequests,
    UE::UAI::Contract::FDiagnostic& OutError, bool bSceneEditing = false,
    const TSet<FString>* PropertyNames = nullptr) {
  UeAgentAuthoringState::FEditablePropertyReadOptions Options;
  Options.SemanticId = SemanticId;
  const auto IdentityScope = MakeShared<FJsonObject>();
  IdentityScope->SetStringField(TEXT("model"), ModelId);
  IdentityScope->SetStringField(TEXT("target_kind"), TargetKind);
  const auto SelectorScope = MakeShared<FJsonObject>();
  for (const auto& Selector : BaseSelectors) SelectorScope->SetStringField(Selector.Key, Selector.Value);
  IdentityScope->SetObjectField(TEXT("selectors"), SelectorScope);
  Options.RequestIdentityScope = UeAgentAuthoringState::MakeStableCanonicalValueKey(MakeShared<FJsonValueObject>(IdentityScope));
  Options.ExcludedProperties = Excluded;
  Options.IncludeProperty = [bSceneEditing, PropertyNames, bComponent = TargetKind == TEXT("level_component_property")](const FProperty* Property, const void* Value, const UObject* Owner) {
    if (PropertyNames && !PropertyNames->Contains(Property->GetName())) return false;
    if (bSceneEditing && !IsSceneEditingProperty(Property->GetName(), bComponent, Owner ? Owner->GetClass() : nullptr)) return false;
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

FLevelSemanticFacts ReadActorFacts(const AActor& Actor, const TMap<FString, FString>* Context) {
  FLevelSemanticFacts Result;
  Result.SemanticId = ActorStateSemanticId;
  Result.Level = Context && Context->Contains(TEXT("actor_level_override"))
      ? Context->FindRef(TEXT("actor_level_override"))
      : Actor.GetLevel() ? Actor.GetLevel()->GetPackage()->GetName() : FString();
  Result.bOmitLabel =
      Context && Context->FindRef(TEXT("actor_label_omit")) == TEXT("true");
  if (!Result.bOmitLabel) {
    Result.Label = Context && Context->Contains(TEXT("actor_label_override"))
            ? Context->FindRef(TEXT("actor_label_override"))
            // Native-default reads must not cache a generated display label on
            // a CDO/archetype: that would change the defaults used by serialization.
            : Actor.GetActorLabel(!Actor.IsTemplate());
  }
  Result.Folder = Context && Context->Contains(TEXT("actor_folder_override"))
          ? Context->FindRef(TEXT("actor_folder_override"))
          : GetActorFolderPathText(Actor);
  Result.Transform = Actor.GetActorTransform();
  ReadTransformOverride(Context, TEXT("actor_"), Result.Transform);
  if (Context) {
    Result.bOmitTranslation = Context->FindRef(TEXT("actor_transform_translation_omit")) == TEXT("true");
    Result.bOmitRotation = Context->FindRef(TEXT("actor_transform_rotation_omit")) == TEXT("true");
    Result.bOmitScale = Context->FindRef(TEXT("actor_transform_scale_omit")) == TEXT("true");
  }
  for (const FName &Tag : Actor.Tags) {
    if (!IsManagedActorIdentityTag(Tag))
      Result.Tags.Add(Tag.ToString());
  }
  const bool bAttachmentOverride =
      Context &&
      Context->FindRef(TEXT("actor_attachment_override")) == TEXT("true");
  const AActor *Parent = Actor.GetAttachParentActor();
  Result.Parent = bAttachmentOverride
          ? Context->FindRef(TEXT("actor_parent_managed_id_override"))
          : (Parent ? GetManagedActorId(*Parent) : FString());
  Result.Socket = bAttachmentOverride
          ? Context->FindRef(TEXT("actor_socket_name_override"))
          : (Actor.GetAttachParentSocketName().IsNone()
                 ? FString()
                 : Actor.GetAttachParentSocketName().ToString());
  return Result;
}

TSharedRef<FJsonObject> BuildActorState(const AActor& Actor, const TMap<FString, FString>* Context = nullptr) {
  return BuildSemanticFactsValue(ReadActorFacts(Actor, Context));
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

FLevelSemanticFacts ReadComponentFacts(const UActorComponent& Component, const TMap<FString, FString>* Context) {
  FLevelSemanticFacts Result;
  Result.SemanticId = ComponentStateSemanticId;
  const USceneComponent *Scene = Cast<USceneComponent>(&Component);
  if (!Scene)
    return Result;
  Result.bScene = true;
  const bool bOverride =
      Context && Context->FindRef(TEXT("attachment_override")) == TEXT("true");
  const USceneComponent *Parent = Scene->GetAttachParent();
  Result.Parent = bOverride && Context->Contains(TEXT("parent_component_override"))
          ? Context->FindRef(TEXT("parent_component_override"))
          : (Parent ? Parent->GetName() : FString());
  Result.Socket = bOverride && Context->Contains(TEXT("socket_name_override"))
          ? Context->FindRef(TEXT("socket_name_override"))
          : (Scene->GetAttachSocketName().IsNone()
                 ? FString()
                 : Scene->GetAttachSocketName().ToString());
  Result.Transform = Scene->GetRelativeTransform();
  ReadTransformOverride(Context, TEXT("component_"), Result.Transform);
  return Result;
}

TSharedRef<FJsonObject> BuildComponentState(const UActorComponent& Component, const TMap<FString, FString>* Context = nullptr) {
  const auto Result = BuildSemanticFactsValue(ReadComponentFacts(Component, Context));
  if (!Context || Context->FindRef(TEXT("scene_editing")) != TEXT("true")) {
    if (const auto Pcg = UeAgentPcgComponentAuthoring::Read(Component)) Result->SetObjectField(TEXT("pcg"), Pcg);
  }
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
  const bool bSpatialState = Request.SemanticId == ActorStateSemanticId || Request.SemanticId == ComponentStateSemanticId;
  if (ActiveSemanticFacts) {
    OutValue.FieldDeclaration = ActiveSemanticFacts->ObjectDeclaration;
    ++ActiveSemanticFacts->TypeDeclarationReads;
    if (bSpatialState) {
      OutValue.ValueDefinition = Request.SemanticId == ComponentStateSemanticId
          ? ActiveSemanticFacts->ComponentDefinition : ActiveSemanticFacts->ActorDefinition;
      ++ActiveSemanticFacts->SpatialDefinitionReads;
    }
    if (!OutValue.FieldDeclaration || (bSpatialState && !OutValue.ValueDefinition)) {
      OutError = StateFailure(TEXT("level_semantic_declaration_missing"),
          TEXT("The active Level semantic-fact scope did not supply its object/spatial declaration; no per-instance reconstruction was selected."), TEXT("semantic_declaration"),
          nullptr, MakeShared<FJsonValueString>(TEXT("declaration produced by the active Level semantic-fact scope")));
      return UeAgentAuthoringState::EReadOutcome::Failed;
    }
  } else if (bSpatialState) {
    // Explicit standalone state reads have no capture fact-set input.
    OutValue.ValueDefinition = BuildSpatialStateDefinition(Request.SemanticId == ComponentStateSemanticId);
  }
  OutValue.bEditable = Request.SemanticId != WorldTargetSemanticId &&
                       Request.SemanticId != ActorTargetSemanticId &&
                       Request.SemanticId != ComponentTargetSemanticId;
  OutError.Reset();
  if (Request.Context.FindRef(TEXT("expected_absent")) == TEXT("true")) {
    OutValue.bAvailable = false;
    OutValue.AvailabilityReason = TEXT("target_not_found");
    return UeAgentAuthoringState::EReadOutcome::Unavailable;
  }

  if (ActiveSemanticFacts) {
    if (const auto Facts = ObserveSemanticFacts(Request)) {
      OutValue.Value = MakeShared<FJsonValueObject>(BuildSemanticFactsValue(*Facts));
      // This builder creates fresh codec-ready JSON, with no borrowed mutable
      // value tree or native buffer. Transfer it and the immutable declaration.
      OutValue.bOwnsNormalizedPropertyData = true;
      RememberSemanticFacts(Request, Facts);
      return UeAgentAuthoringState::EReadOutcome::Resolved;
    }
  }

  if (Request.SemanticId == WorldStateSemanticId ||
      Request.SemanticId == WorldTargetSemanticId) {
    const UWorld *World = Cast<UWorld>(Request.RootObject);
    if (!World) {
      OutValue.bAvailable = false;
      OutValue.AvailabilityReason = TEXT("world_not_found");
      return UeAgentAuthoringState::EReadOutcome::Unavailable;
    }
    TSharedPtr<FJsonObject> State;
    if (Request.SemanticId == WorldStateSemanticId) {
      if (const FString* Override = Request.Context.Find(TEXT("world_topology_override")))
        FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(*Override), State);
    }
    OutValue.Value = MakeShared<FJsonValueObject>(State.IsValid() ? State.ToSharedRef()
        : Request.SemanticId == WorldTargetSemanticId ? BuildWorldTarget(*World) : CaptureWorldTopology(*World));
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

FScopedLevelSemanticFacts::FScopedLevelSemanticFacts()
    : Facts(MakeShared<FLevelSemanticFactsSet>()), Previous(ActiveSemanticFacts) {
  Facts->ActorDefinition = BuildSpatialStateDefinition(false);
  Facts->ComponentDefinition = BuildSpatialStateDefinition(true);
  Facts->ActorDefinitionKey = UeAgentAuthoringState::MakeStableCanonicalValueKey(Facts->ActorDefinition);
  Facts->ComponentDefinitionKey = UeAgentAuthoringState::MakeStableCanonicalValueKey(Facts->ComponentDefinition);
  UeAgentAuthoringTypeDescriptors::FDescriptor Type; Type.Kind = TEXT("object");
  Facts->ObjectDeclaration = MakeShared<FJsonObject>();
  Facts->ObjectDeclaration->SetObjectField(TEXT("type"), Type.ToJsonObject());
  ActiveSemanticFacts = Facts.Get();
}
FScopedLevelSemanticFacts::~FScopedLevelSemanticFacts() {
  ActiveSemanticFacts = Previous;
  UE_LOG(LogTemp, Display, TEXT("UAI export timing phase=level_semantic_declarations actor_definitions=1 component_definitions=1 object_declarations=1 type_reads=%lld spatial_reads=%lld per_instance_spatial_builds=0"),
      Facts->TypeDeclarationReads, Facts->SpatialDefinitionReads);
}
TSharedPtr<const FLevelSemanticFactsSet> FScopedLevelSemanticFacts::GetFacts() const { return Facts; }
bool FScopedLevelSemanticFacts::ObserveUnchanged(const UeAgentAuthoringState::FReadRequest& Request,
    const TSharedPtr<const FLevelSemanticFactsSet>& Baseline) {
  if (!Baseline) return false;
  if (Request.SemanticId == ActorStateSemanticId && Baseline->ActorDefinitionKey != Facts->ActorDefinitionKey) return false;
  if (Request.SemanticId == ComponentStateSemanticId && Baseline->ComponentDefinitionKey != Facts->ComponentDefinitionKey) return false;
  const auto* PreviousFacts = Baseline->Values.Find(SemanticFactsKey(Request));
  if (!PreviousFacts) return false;
  const auto Current = ObserveSemanticFacts(Request);
  if (!Current || !SameSemanticFacts(**PreviousFacts, *Current)) return false;
  RememberSemanticFacts(Request, Current);
  return true;
}

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
  for (TActorIterator<AActor> It(World, AActor::StaticClass(), EActorIteratorFlags::SkipPendingKill); It; ++It) {
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
  for (TActorIterator<AActor> It(&World, AActor::StaticClass(), EActorIteratorFlags::SkipPendingKill); It; ++It) {
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
  for (TActorIterator<AActor> It(&World, AActor::StaticClass(), EActorIteratorFlags::SkipPendingKill); It; ++It) {
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

bool BuildWorldExportIdentityReadRequests(
    UWorld &World, TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
    UE::UAI::Contract::FDiagnostic& OutError) {
  OutRequests.Reset();
  UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
  return BuildWorldIdentity(World, Identity, OutError) &&
      AddReadRequest(WorldTargetSemanticId, Identity, &World, false, OutRequests);
}

bool BuildWorldExportReadRequests(
    UWorld &World, TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
    UE::UAI::Contract::FDiagnostic& OutError, const TSet<FString>* PropertyNames,
    bool bWorldState) {
  if (!BuildWorldExportIdentityReadRequests(World, OutRequests, OutError)) return false;
  if (bWorldState && !AddReadRequest(WorldStateSemanticId, OutRequests[0].TargetIdentity, &World, false,
                      OutRequests)) return false;
  if (PropertyNames && PropertyNames->IsEmpty()) return true;
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
      OutError, false, PropertyNames);
}

static FName SceneEditingRootProperty(const FString& PropertyPath) {
  int32 End = PropertyPath.Len();
  int32 Index;
  if (PropertyPath.FindChar(TEXT('.'), Index)) End = FMath::Min(End, Index);
  if (PropertyPath.FindChar(TEXT('['), Index)) End = FMath::Min(End, Index);
  return FName(*PropertyPath.Left(End));
}

static UClass* SceneEditingPropertyClass(const FName Name, bool bComponent) {
  if (bComponent && Name == TEXT("Intensity")) return ULightComponentBase::StaticClass();
  if (bComponent && Name == TEXT("AttenuationRadius")) return ULocalLightComponent::StaticClass();
  if (!bComponent && Name == TEXT("Settings")) return APostProcessVolume::StaticClass();
  return nullptr;
}

bool RequiresSceneEditingClassCheck(const FString& PropertyPath, bool bComponent) {
  return SceneEditingPropertyClass(SceneEditingRootProperty(PropertyPath), bComponent) != nullptr;
}

bool IsSceneEditingProperty(const FString& PropertyPath, bool bComponent) {
  const FName Name = SceneEditingRootProperty(PropertyPath);
  if (!bComponent) return Name == TEXT("bHidden");
  static const TSet<FName> Properties = {
      TEXT("StaticMesh"), TEXT("SkeletalMeshAsset"), TEXT("OverrideMaterials"),
      TEXT("Mobility"), TEXT("bVisible"), TEXT("bHiddenInGame"), TEXT("CastShadow")};
  return Properties.Contains(Name);
}

bool IsSceneEditingProperty(const FString& PropertyPath, bool bComponent, const UClass* TargetClass) {
  if (IsSceneEditingProperty(PropertyPath, bComponent)) return true;
  const FName Name = SceneEditingRootProperty(PropertyPath);
  const UClass* RequiredClass = SceneEditingPropertyClass(Name, bComponent);
  if (!RequiredClass || !TargetClass || !TargetClass->HasAnyClassFlags(CLASS_Native)
      || TargetClass->ClassGeneratedBy || !TargetClass->GetPathName().StartsWith(TEXT("/Script/Engine."))
      || !TargetClass->IsChildOf(RequiredClass)) return false;
  const FProperty* Property = FindFProperty<FProperty>(TargetClass, Name);
  return Property && Property->GetOwnerClass() == RequiredClass;
}

template <typename TRequest>
static bool BuildActorExportReadRequestsImpl(
    AActor &Actor, TArray<TRequest> &OutRequests,
    UE::UAI::Contract::FDiagnostic& OutError, bool bSceneEditing,
    bool bIncludeState = true, bool bIncludeProperties = true) {
  OutRequests.Reset();
  UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
  const TMap<FString, FString> Selectors = ActorSelectors(Actor);
  if (!BuildIdentity(ActorIdentityModelId, TEXT("level_actor"), Selectors,
                     Identity, OutError) ||
      !AddReadRequest(ActorTargetSemanticId, Identity, &Actor, false,
                      OutRequests) ||
      (bIncludeState && !AddReadRequest(ActorStateSemanticId, Identity, &Actor, false,
                      OutRequests))) {
    return false;
  }
  return !bIncludeProperties || AppendEditableProperties(
      &Actor, ActorPropertySemanticId, ActorPropertyIdentityModelId,
      TEXT("level_actor_property"), Selectors,
      {TEXT("ActorGuid"), TEXT("RootComponent"), TEXT("Tags")}, OutRequests,
      OutError, bSceneEditing);
}

bool BuildActorExportReadRequests(AActor& Actor, TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
    UE::UAI::Contract::FDiagnostic& OutError, bool bSceneEditing) {
  return BuildActorExportReadRequestsImpl(Actor, OutRequests, OutError, bSceneEditing);
}
bool BuildActorExportReadRequests(AActor& Actor, TArray<UeAgentAuthoringState::FReadRequestBinding>& OutRequests,
    UE::UAI::Contract::FDiagnostic& OutError, bool bSceneEditing) {
  return BuildActorExportReadRequestsImpl(Actor, OutRequests, OutError, bSceneEditing);
}
bool BuildActorExportScopedReadRequests(AActor& Actor, TArray<UeAgentAuthoringState::FReadRequestBinding>& OutRequests,
    UE::UAI::Contract::FDiagnostic& OutError, bool bSceneEditing, bool bIncludeState, bool bIncludeProperties) {
  return BuildActorExportReadRequestsImpl(Actor, OutRequests, OutError, bSceneEditing, bIncludeState, bIncludeProperties);
}
bool BuildActorExportScopedReadRequests(AActor& Actor, TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
    UE::UAI::Contract::FDiagnostic& OutError, bool bSceneEditing, bool bIncludeState, bool bIncludeProperties) {
  return BuildActorExportReadRequestsImpl(Actor, OutRequests, OutError, bSceneEditing, bIncludeState, bIncludeProperties);
}

TSet<FName> ComponentStructuralProperties(const UActorComponent& Component) {
  TSet<FName> Excluded =
      {TEXT("AttachParent"), TEXT("AttachSocketName"), TEXT("RelativeLocation"),
       TEXT("RelativeRotation"), TEXT("RelativeScale3D"), TEXT("CreationMethod")};
  if (Component.IsA<UPCGComponent>()) Excluded.Add(TEXT("GraphInstance"));
  if (Component.GetOwner() && Component.GetOwner()->IsA<AWorldPartitionHLOD>())
    Excluded.Add(TEXT("bVisible"));
  return Excluded;
}

template <typename TRequest>
static bool BuildComponentExportReadRequestsImpl(
    UActorComponent &Component,
    TArray<TRequest> &OutRequests,
    UE::UAI::Contract::FDiagnostic& OutError, bool bSceneEditing, bool bIncludeProperties = true) {
  OutRequests.Reset();
  UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
  const TMap<FString, FString> Selectors = ComponentSelectors(Component);
  if (!BuildIdentity(ComponentIdentityModelId, TEXT("level_component"),
                     Selectors, Identity, OutError) ||
      !AddReadRequest(ComponentTargetSemanticId, Identity, &Component, false,
                      OutRequests, bSceneEditing) ||
      !AddReadRequest(ComponentStateSemanticId, Identity, &Component, false,
                      OutRequests, bSceneEditing)) {
    return false;
  }
  return !bIncludeProperties || AppendEditableProperties(
      &Component, ComponentPropertySemanticId, ComponentPropertyIdentityModelId,
      TEXT("level_component_property"), Selectors, ComponentStructuralProperties(Component),
      OutRequests, OutError, bSceneEditing);
}

bool BuildComponentExportReadRequests(UActorComponent& Component, TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
    UE::UAI::Contract::FDiagnostic& OutError, bool bSceneEditing) {
  return BuildComponentExportReadRequestsImpl(Component, OutRequests, OutError, bSceneEditing);
}
bool BuildComponentExportReadRequests(UActorComponent& Component, TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
    UE::UAI::Contract::FDiagnostic& OutError, bool bSceneEditing, bool bIncludeProperties) {
  return BuildComponentExportReadRequestsImpl(Component, OutRequests, OutError, bSceneEditing, bIncludeProperties);
}
bool BuildComponentExportReadRequests(UActorComponent& Component, TArray<UeAgentAuthoringState::FReadRequestBinding>& OutRequests,
    UE::UAI::Contract::FDiagnostic& OutError, bool bSceneEditing, bool bIncludeProperties) {
  return BuildComponentExportReadRequestsImpl(Component, OutRequests, OutError, bSceneEditing, bIncludeProperties);
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
