// Copyright Epic Games, Inc. All Rights Reserved.
#include "Domains/Level/Authoring/UeAgentLevelWorldTopology.h"

#include "Domains/Level/Authoring/UeAgentLevelAuthoringSpatial.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "DataLayer/DataLayerEditorSubsystem.h"
#include "Editor.h"
#include "EditorLevelUtils.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "Engine/LevelStreamingDynamic.h"
#include "Engine/World.h"
#include "LevelUtils.h"
#include "Misc/PackageName.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/DataLayer/DataLayerAsset.h"
#include "WorldPartition/DataLayer/DataLayerInstance.h"
#include "WorldPartition/DataLayer/WorldDataLayers.h"

namespace UeAgentLevelAuthoring {
namespace {
bool Fail(UE::UAI::Contract::FDiagnostic& Error, const FJsonObject& Input,
    const FString& Field, const FString& Reason) {
  Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("level_world_topology_invalid"), Reason, TEXT("level_apply"));
  Error.PropertyPath = Field;
  Error.ActualValue = Input.TryGetField(Field);
  Error.ActualValuePresent = Error.ActualValue.IsValid();
  UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, Input, Field);
  return false;
}

ULevelStreaming* FindStreaming(const UWorld& World, const FString& Package) {
  for (ULevelStreaming* Streaming : World.GetStreamingLevels())
    if (Streaming && Streaming->GetWorldAssetPackageName() == Package) return Streaming;
  return nullptr;
}

TSharedRef<FJsonObject> CaptureStreaming(const ULevelStreaming& Streaming) {
  auto Row = MakeShared<FJsonObject>();
  Row->SetStringField(TEXT("streaming_class"), Streaming.GetClass()->GetPathName());
  Row->SetBoolField(TEXT("editor_visible"), Streaming.GetShouldBeVisibleInEditor());
  Row->SetObjectField(TEXT("transform"), BuildTransformState(Streaming.LevelTransform, ETransformFrame::ActorWorld));
  if (const auto* Dynamic = Cast<ULevelStreamingDynamic>(&Streaming)) {
    Row->SetBoolField(TEXT("initially_loaded"), Dynamic->bInitiallyLoaded);
    Row->SetBoolField(TEXT("initially_visible"), Dynamic->bInitiallyVisible);
  }
  return Row;
}

FString RuntimeStateName(EDataLayerRuntimeState State) {
  return State == EDataLayerRuntimeState::Activated ? TEXT("Activated")
      : State == EDataLayerRuntimeState::Loaded ? TEXT("Loaded") : TEXT("Unloaded");
}

UDataLayerInstance* FindDataLayer(const UWorld& World, const FString& AssetPath) {
  UDataLayerInstance* Result = nullptr;
  if (AWorldDataLayers* Layers = World.GetWorldDataLayers())
    Layers->ForEachDataLayerInstance([&](UDataLayerInstance* Layer) {
      if (Layer->GetAsset() && Layer->GetAsset()->GetPathName() == AssetPath) Result = Layer;
      return Result == nullptr;
    });
  return Result;
}

TSharedRef<FJsonObject> CaptureDataLayer(const UDataLayerInstance& Layer) {
  auto Row = MakeShared<FJsonObject>();
  Row->SetBoolField(TEXT("initially_visible"), Layer.IsInitiallyVisible());
  if (Layer.IsRuntime()) Row->SetStringField(TEXT("initial_runtime_state"), RuntimeStateName(Layer.GetInitialRuntimeState()));
  return Row;
}
}

TSharedRef<FJsonObject> CaptureWorldTopology(const UWorld& World) {
  auto State = MakeShared<FJsonObject>();
  auto Levels = MakeShared<FJsonObject>();
  for (const ULevelStreaming* Streaming : World.GetStreamingLevels())
    if (Streaming) Levels->SetObjectField(Streaming->GetWorldAssetPackageName(), CaptureStreaming(*Streaming));
  State->SetObjectField(TEXT("streaming_levels"), Levels);
  if (const UWorldPartition* Partition = World.GetWorldPartition()) {
    auto Settings = MakeShared<FJsonObject>();
    Settings->SetBoolField(TEXT("streaming_enabled"), Partition->IsStreamingEnabled());
    State->SetObjectField(TEXT("world_partition"), Settings);
    auto Layers = MakeShared<FJsonObject>();
    if (AWorldDataLayers* NativeLayers = World.GetWorldDataLayers())
      NativeLayers->ForEachDataLayerInstance([&](UDataLayerInstance* Layer) {
        if (Layer->GetAsset() && Layer->GetAsset()->GetClass() == UDataLayerAsset::StaticClass())
          Layers->SetObjectField(Layer->GetAsset()->GetPathName(), CaptureDataLayer(*Layer));
        return true;
      });
    State->SetObjectField(TEXT("data_layers"), Layers);
  }
  return State;
}

ULevel* FindAuthoringLevel(UWorld& World, const FString& PackageName) {
  if (World.PersistentLevel && World.GetPackage()->GetName() == PackageName) return World.PersistentLevel;
  ULevelStreaming* Streaming = FindStreaming(World, PackageName);
  return Streaming ? Streaming->GetLoadedLevel() : nullptr;
}

bool FWorldTopologyEdit::Prepare(UWorld& World, const TSharedPtr<const FJsonObject>& Extension,
    UE::UAI::Contract::FDiagnostic& Error) {
  if (!Extension.IsValid() || Extension->Values.IsEmpty()) return true;
  Before = CaptureWorldTopology(World);
  Desired = CaptureWorldTopology(World);
  for (const auto& Field : Extension->Values)
    if (Field.Key != TEXT("streaming_levels") && Field.Key != TEXT("world_partition") && Field.Key != TEXT("data_layers"))
      return Fail(Error, *Extension, Field.Key, TEXT("Unknown Level world state field."));
  if (Extension->HasField(TEXT("streaming_levels"))) {
    const TSharedPtr<FJsonObject>* Levels = nullptr;
    if (!Extension->TryGetObjectField(TEXT("streaming_levels"), Levels))
      return Fail(Error, *Extension, TEXT("streaming_levels"), TEXT("Streaming levels must be keyed by map package path."));
    if (World.GetWorldPartition() && !(*Levels)->Values.IsEmpty())
      return Fail(Error, *Extension, TEXT("streaming_levels"), TEXT("Legacy streaming relations require a non-partitioned World."));
    auto Normalized = MakeShared<FJsonObject>();
    for (const auto& Pair : (*Levels)->Values) {
      // Workspace omissions preserve baseline state. An explicit null row is
      // the request to unlink a map while retaining its package and contents.
      if (Pair.Value->Type == EJson::Null) continue;
      if (!FPackageName::IsValidLongPackageName(Pair.Key) || Pair.Key == World.GetPackage()->GetName()
          || !FPackageName::DoesPackageExist(Pair.Key) || Pair.Value->Type != EJson::Object)
        return Fail(Error, **Levels, Pair.Key, TEXT("Each relation must reference an existing distinct map package and contain an object."));
      UWorld* Map = LoadObject<UWorld>(nullptr, *(Pair.Key + TEXT(".") + FPackageName::GetShortName(Pair.Key)));
      if (!Map || !Map->PersistentLevel || Map->GetWorldPartition())
        return Fail(Error, **Levels, Pair.Key, TEXT("A streaming relation requires an existing non-partitioned map."));
      ULevelStreaming* Existing = FindStreaming(World, Pair.Key);
      auto Row = Existing ? CaptureStreaming(*Existing) : MakeShared<FJsonObject>();
      if (!Existing) {
        Row->SetStringField(TEXT("streaming_class"), ULevelStreamingDynamic::StaticClass()->GetPathName());
        Row->SetBoolField(TEXT("editor_visible"), true);
        Row->SetObjectField(TEXT("transform"), BuildTransformState(FTransform::Identity, ETransformFrame::ActorWorld));
      }
      const auto Input = Pair.Value->AsObject();
      for (const auto& Field : Input->Values) {
        if (Field.Key != TEXT("streaming_class") && Field.Key != TEXT("editor_visible")
            && Field.Key != TEXT("initially_loaded") && Field.Key != TEXT("initially_visible") && Field.Key != TEXT("transform"))
          return Fail(Error, *Input, Field.Key, TEXT("Unknown streaming relation field; Editor loaded state is not a persistent streaming setting."));
        Row->SetField(Field.Key, Field.Value);
      }
      FString ClassPath;
      if (!Row->TryGetStringField(TEXT("streaming_class"), ClassPath))
        return Fail(Error, *Row, TEXT("streaming_class"), TEXT("Expected a LevelStreaming class path."));
      UClass* Class = StaticLoadClass(ULevelStreaming::StaticClass(), nullptr, *ClassPath);
      if (!Class || Class->HasAnyClassFlags(CLASS_Abstract) || (Existing && Existing->GetClass() != Class))
        return Fail(Error, *Row, TEXT("streaming_class"), TEXT("Expected a concrete LevelStreaming class; changing an existing relation's class requires removing and re-adding it."));
      const auto* Defaults = Cast<ULevelStreamingDynamic>(Class->GetDefaultObject());
      for (const TCHAR* Field : {TEXT("editor_visible"), TEXT("initially_loaded"), TEXT("initially_visible")}) {
        bool Value;
        if (Row->HasField(Field) && !Row->TryGetBoolField(Field, Value))
          return Fail(Error, *Row, Field, TEXT("Expected a boolean."));
      }
      if (!Defaults && (Row->HasField(TEXT("initially_loaded")) || Row->HasField(TEXT("initially_visible"))))
        return Fail(Error, *Row, TEXT("streaming_class"), TEXT("Initial loaded/visible flags belong to LevelStreamingDynamic subclasses."));
      if (Defaults) {
        if (!Row->HasField(TEXT("initially_loaded"))) Row->SetBoolField(TEXT("initially_loaded"), Defaults->bInitiallyLoaded);
        if (!Row->HasField(TEXT("initially_visible"))) Row->SetBoolField(TEXT("initially_visible"), Defaults->bInitiallyVisible);
      }
      const TSharedPtr<FJsonObject>* Transform = nullptr;
      FTransformPatch Patch;
      if (!Row->TryGetObjectField(TEXT("transform"), Transform)
          || !ParseTransformPatch(*Transform, ETransformFrame::ActorWorld, Patch, Error)) return false;
      FTransform Value = Existing ? Existing->LevelTransform : FTransform::Identity;
      ApplyTransformPatch(Patch, Value);
      Row->SetObjectField(TEXT("transform"), BuildTransformState(Value, ETransformFrame::ActorWorld));
      Normalized->SetObjectField(Pair.Key, Row);
    }
    Desired->SetObjectField(TEXT("streaming_levels"), Normalized);
  }
  if (Extension->HasField(TEXT("data_layers"))) {
    const TSharedPtr<FJsonObject>* Input = nullptr;
    if (!World.GetWorldPartition() || !Extension->TryGetObjectField(TEXT("data_layers"), Input))
      return Fail(Error, *Extension, TEXT("data_layers"), TEXT("Data Layer instances require a partitioned map and an object keyed by asset object path."));
    auto Layers = Desired->GetObjectField(TEXT("data_layers"));
    for (const auto& Pair : (*Input)->Values) {
      UDataLayerAsset* Asset = LoadObject<UDataLayerAsset>(nullptr, *Pair.Key);
      if (!Asset || Asset->GetClass() != UDataLayerAsset::StaticClass() || Asset->GetPathName() != Pair.Key || Pair.Value->Type != EJson::Object)
        return Fail(Error, **Input, Pair.Key, TEXT("Each entry requires an existing DataLayerAsset object path and state object. Private/external layers and deletion are not supported by this field."));
      UDataLayerInstance* Existing = FindDataLayer(World, Pair.Key);
      if (Existing && Existing->IsReadOnly())
        return Fail(Error, **Input, Pair.Key, TEXT("The selected Data Layer instance is read-only."));
      auto Row = Existing ? CaptureDataLayer(*Existing) : MakeShared<FJsonObject>();
      if (!Existing) {
        Row->SetBoolField(TEXT("initially_visible"), true);
        if (Asset->IsRuntime()) Row->SetStringField(TEXT("initial_runtime_state"), TEXT("Unloaded"));
      }
      const auto Fields = Pair.Value->AsObject();
      for (const auto& Field : Fields->Values) {
        if (Field.Key == TEXT("initially_visible")) {
          bool Value;
          if (!Field.Value->TryGetBool(Value)) return Fail(Error, *Fields, Field.Key, TEXT("Expected a boolean."));
        } else if (Field.Key == TEXT("initial_runtime_state")) {
          FString Value;
          if (!Asset->IsRuntime() || !Field.Value->TryGetString(Value)
              || (Value != TEXT("Unloaded") && Value != TEXT("Loaded") && Value != TEXT("Activated")))
            return Fail(Error, *Fields, Field.Key, TEXT("A Runtime Data Layer requires Unloaded, Loaded or Activated."));
        } else return Fail(Error, *Fields, Field.Key, TEXT("Unknown persistent Data Layer state. Temporary loaded/visible state belongs to the Editor session."));
        Row->SetField(Field.Key, Field.Value);
      }
      Layers->SetObjectField(Pair.Key, Row);
    }
  }
  if (Extension->HasField(TEXT("world_partition"))) {
    const TSharedPtr<FJsonObject>* Input = nullptr;
    if (!World.GetWorldPartition() || !Extension->TryGetObjectField(TEXT("world_partition"), Input))
      return Fail(Error, *Extension, TEXT("world_partition"), TEXT("World Partition settings require an existing partitioned map."));
    auto Settings = Desired->GetObjectField(TEXT("world_partition"));
    for (const auto& Pair : (*Input)->Values) {
      if (Pair.Key == TEXT("streaming_enabled")) {
        bool Value;
        if (!Pair.Value->TryGetBool(Value)) return Fail(Error, **Input, Pair.Key, TEXT("Expected a boolean."));
      } else return Fail(Error, **Input, Pair.Key, TEXT("Unknown World Partition setting."));
      Settings->SetField(Pair.Key, Pair.Value);
    }
  }
  return true;
}

bool FWorldTopologyEdit::ApplyState(UWorld& World, const TSharedPtr<FJsonObject>& State,
    UE::UAI::Contract::FDiagnostic& Error) {
  if (!State.IsValid()) return true;
  if (const TSharedPtr<FJsonObject>* Layers = nullptr; State->TryGetObjectField(TEXT("data_layers"), Layers)) {
    UDataLayerEditorSubsystem* EditorLayers = GEditor ? GEditor->GetEditorSubsystem<UDataLayerEditorSubsystem>() : nullptr;
    if (!EditorLayers) return Fail(Error, *State, TEXT("data_layers"), TEXT("The Data Layer Editor subsystem is unavailable."));
    for (const auto& Pair : (*Layers)->Values) {
      UDataLayerInstance* Layer = FindDataLayer(World, Pair.Key);
      if (!Layer) {
        FDataLayerCreationParameters Parameters;
        Parameters.DataLayerAsset = LoadObject<UDataLayerAsset>(nullptr, *Pair.Key);
        Parameters.WorldDataLayers = World.GetWorldDataLayers();
        Layer = EditorLayers->CreateDataLayerInstance(Parameters);
        if (!Layer) return Fail(Error, **Layers, Pair.Key, TEXT("Unreal could not create the Data Layer instance."));
        CreatedDataLayers.Add(Layer);
      }
      const auto Row = Pair.Value->AsObject();
      FString RuntimeState;
      const bool bHasRuntimeState = Row->TryGetStringField(TEXT("initial_runtime_state"), RuntimeState);
      const bool bInitiallyVisible = Row->GetBoolField(TEXT("initially_visible"));
      // SetInitialRuntimeState only assigns the native value. Enlist the
      // existing instance's package as dirty as well as newly created layers.
      if (Layer->IsInitiallyVisible() != bInitiallyVisible
          || (bHasRuntimeState && RuntimeStateName(Layer->GetInitialRuntimeState()) != RuntimeState))
        Layer->Modify();
      Layer->SetIsInitiallyVisible(bInitiallyVisible);
      if (bHasRuntimeState)
        Layer->SetInitialRuntimeState(RuntimeState == TEXT("Activated") ? EDataLayerRuntimeState::Activated
            : RuntimeState == TEXT("Loaded") ? EDataLayerRuntimeState::Loaded : EDataLayerRuntimeState::Unloaded);
    }
  }
  const auto Levels = State->GetObjectField(TEXT("streaming_levels"));
  World.Modify();
  const auto Current = World.GetStreamingLevels();
  for (ULevelStreaming* Streaming : Current) {
    if (Streaming && !Levels->HasField(Streaming->GetWorldAssetPackageName())) {
      Removed.Add(Streaming->GetWorldAssetPackageName(), Streaming);
      if (ULevel* Loaded = Streaming->GetLoadedLevel(); Loaded && Loaded->bAlreadyMovedActors) {
        FLevelUtils::RemoveEditorTransform(Streaming);
        Loaded->bAlreadyMovedActors = false;
      }
      // Keep loaded objects alive until the shared durable transaction saves all
      // affected packages and detaches the World. The subsequent native reload
      // proves removal without discarding a dirty sub-map or invalidating peers.
      World.RemoveStreamingLevel(Streaming);
    }
  }
  for (const auto& Pair : Levels->Values) {
    const auto Row = Pair.Value->AsObject();
    ULevelStreaming* Streaming = FindStreaming(World, Pair.Key);
    if (!Streaming) {
      if (const auto* Previous = Removed.Find(Pair.Key); Previous && Previous->IsValid()) {
        Streaming = Previous->Get();
        World.AddStreamingLevel(Streaming);
        if (ULevel* Loaded = Streaming->GetLoadedLevel(); Loaded && !Loaded->bAlreadyMovedActors) {
          FLevelUtils::ApplyEditorTransform(Streaming);
          Loaded->bAlreadyMovedActors = true;
        }
      } else {
        UClass* Class = StaticLoadClass(ULevelStreaming::StaticClass(), nullptr, *Row->GetStringField(TEXT("streaming_class")));
        Streaming = UEditorLevelUtils::AddLevelToWorld(&World, *Pair.Key, Class);
      }
    }
    if (!Streaming || !Streaming->GetLoadedLevel())
      return Fail(Error, *Levels, Pair.Key, TEXT("Unreal could not load the requested streaming map."));
    Streaming->Modify();
    FTransformPatch Patch;
    if (!ParseTransformPatch(Row->GetObjectField(TEXT("transform")), ETransformFrame::ActorWorld, Patch, Error)) return false;
    FTransform Transform = Streaming->LevelTransform;
    ApplyTransformPatch(Patch, Transform);
    if (!Transform.Equals(Streaming->LevelTransform)) FLevelUtils::SetEditorTransform(Streaming, Transform);
    if (auto* Dynamic = Cast<ULevelStreamingDynamic>(Streaming)) {
      Dynamic->bInitiallyLoaded = Row->GetBoolField(TEXT("initially_loaded"));
      Dynamic->bInitiallyVisible = Row->GetBoolField(TEXT("initially_visible"));
    }
    const bool Visible = Row->GetBoolField(TEXT("editor_visible"));
    Streaming->SetShouldBeVisibleInEditor(Visible);
    UEditorLevelUtils::SetLevelVisibility(Streaming->GetLoadedLevel(), Visible, false);
  }
  if (const TSharedPtr<FJsonObject>* Settings = nullptr; State->TryGetObjectField(TEXT("world_partition"), Settings)) {
    UWorldPartition* Partition = World.GetWorldPartition();
    Partition->Modify();
    Partition->SetEnableStreaming((*Settings)->GetBoolField(TEXT("streaming_enabled")));
  }
  World.MarkPackageDirty();
  return true;
}

bool FWorldTopologyEdit::Apply(UWorld& World, UE::UAI::Contract::FDiagnostic& Error) { return ApplyState(World, Desired, Error); }
bool FWorldTopologyEdit::Restore(UWorld& World, UE::UAI::Contract::FDiagnostic& Error) {
  if (UDataLayerEditorSubsystem* Layers = GEditor ? GEditor->GetEditorSubsystem<UDataLayerEditorSubsystem>() : nullptr)
    for (const auto& Created : CreatedDataLayers) if (Created.IsValid()) Layers->DeleteDataLayer(Created.Get());
  CreatedDataLayers.Reset();
  return ApplyState(World, Before, Error);
}
void FWorldTopologyEdit::Release() { Removed.Reset(); CreatedDataLayers.Reset(); }
}
