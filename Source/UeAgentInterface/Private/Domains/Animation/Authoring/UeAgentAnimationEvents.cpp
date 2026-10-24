#include "Domains/Animation/Authoring/UeAgentAnimationEvents.h"

#include "Animation/AnimSequence.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimTypes.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Core/HjsonWorkspace/UeAgentKeyedItemAuthoringProjection.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringJsonFields.h"
#include "Utilities/AuthoringObjectApply/UeAgentAuthoringObjectApply.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Utilities/AuthoringCore/UeAgentTimelineValueJson.h"
#include "Utilities/AuthoringCore/UeAgentMovieSceneAuthoring.h"
#include <type_traits>

namespace UeAgentAnimationAuthoring {
namespace {
using namespace UE::UAI;

void MarkNativeReadFailure(Contract::FDiagnostic& Diagnostic) {
  for (auto& Child : Diagnostic.Diagnostics) MarkNativeReadFailure(Child);
  if (Diagnostic.PublicCause) {
    auto Cause = MakeShared<Contract::FDiagnostic>(*Diagnostic.PublicCause);
    MarkNativeReadFailure(*Cause); Diagnostic.PublicCause = MoveTemp(Cause);
  }
  if (Diagnostic.Source.SourceFile.IsEmpty()
      && (Diagnostic.Source.SourceUnit.IsEmpty() || Diagnostic.Source.InternalJsonPath.IsEmpty()))
    Diagnostic.SourceKind = TEXT("native_result");
}

bool Bind(const TSharedPtr<FJsonValue>& Value, int32 Count,
          TArray<HjsonWorkspace::FOrderedStructuredItemBinding>& Items, UE::UAI::Contract::FDiagnostic& Error) {
  auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
  StateSemantic::FUeAgentCanonicalValuePtr Desired;
  if (!StateSemantic::CanonicalJson::TryFromJsonValue(Value, Resources, Desired, Error)) return false;
  HjsonWorkspace::FWorkspaceError Failure;
  if (!HjsonWorkspace::TryResolveOrderedStructuredItems(*Desired, Count, Resources, Items, Failure)) {
    Error = Failure.GetDiagnostic();
    return false;
  }
  return true;
}

TSharedPtr<FJsonObject> Row(int32 Index, UE::UAI::Contract::FDiagnostic& Error) {
  FString Key;
  HjsonWorkspace::FWorkspaceError Failure;
  if (!HjsonWorkspace::TryFormatReadableKeyedItemOrdinal(TEXT("Entry"), Index, Key, Failure)) {
    Error = Failure.GetDiagnostic();
    return nullptr;
  }
  auto Result = MakeShared<FJsonObject>();
  Result->SetStringField(TEXT("instance"), Key);
  return Result;
}

bool IsRecordField(const FProperty* Field) {
  if (!Field) return false;
  const UStruct* Owner = Field->GetOwnerStruct();
  return Owner == FAnimNotifyTrack::StaticStruct() ||
      (Owner == FAnimSyncMarker::StaticStruct() && Field->GetFName() != TEXT("Guid"));
}

bool ApplyFields(UScriptStruct* Type, void* Data, UAnimSequenceBase& Asset,
                 const TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& Error, const FString& CanonicalAssetPath) {
  const TSharedPtr<FJsonObject>* Object = nullptr;
  const TSharedPtr<FJsonObject>* Fields = nullptr;
  if (!Value || !Value->TryGetObject(Object) || !Object ||
      !(*Object)->TryGetObjectField(TEXT("fields"), Fields) || !Fields) {
    const bool bObject = Object && Object->IsValid();
    Error = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("animation_event_fields_expected"),
        bObject ? TEXT("/settings/fields") : TEXT("/settings"), MakeShared<FJsonValueString>(TEXT("object")),
        bObject ? (*Object)->TryGetField(TEXT("fields")) : Value, TEXT("authoring"),
        bObject ? TEXT("Animation event settings require a fields object.") : TEXT("Animation event settings must be an object."));
    return false;
  }
  TArray<TSharedPtr<FJsonValue>> Descriptors;
  for (const auto& Field : (*Fields)->Values) {
    auto Descriptor = MakeShared<FJsonObject>();
    Descriptor->SetStringField(TEXT("property_path"), Field.Key);
    Descriptor->SetField(TEXT("value"), Field.Value);
    Descriptors.Add(MakeShared<FJsonValueObject>(Descriptor));
  }
  UeAgentAuthoringObjectApply::FOptions Options;
  Options.DiagnosticTargetPath = CanonicalAssetPath;
  Options.bCompactValues = true;
  Options.bEmitEditorNotifications = false;
  Options.bMarkPackageDirty = false;
  Options.AuthorizeProperty = IsRecordField;
  Options.IncludeProperty = IsRecordField;
  UeAgentAuthoringObjectApply::FPreparedApply Prepared;
  UeAgentAuthoringObjectApply::FFailure Failure;
  UeAgentAuthoringObjectApply::FApplyResult Applied;
  if (!UeAgentAuthoringObjectApply::PrepareStruct(Type, Data, &Asset, Descriptors, Options, Prepared, Failure)) {
    Error = Failure.Diagnostic;
    UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error, TEXT("/settings/fields"), true);
    return false;
  }
  if (!UeAgentAuthoringObjectApply::ApplyPrepared(Prepared, Applied)) {
    Error = Applied.Diagnostic;
    UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error, TEXT("/settings/fields"), true);
    return false;
  }
  return true;
}

bool ReadNotify(UAnimSequenceBase& Asset, const FAnimNotifyEvent& Notify,
                UeAgentAuthoringState::FStateSnapshot& Snapshot,
                TSharedPtr<FJsonObject>& Properties, UE::UAI::Contract::FDiagnostic& Error) {
  UeAgentDetailsAuthoringService::FCapturePolicy Policy;
  // Time and linkage are native operations, never reflected LinkValue edits.
  Policy.ExcludedProperties = {TEXT("SlotIndex"), TEXT("LinkMethod"), TEXT("LinkedSequence")};
  if (UeAgentDetailsAuthoringService::CaptureStructProperties(FAnimNotifyEvent::StaticStruct(), &Notify,
      Asset.GetPathName(), &Asset, Policy, Snapshot, Properties, Error) &&
      UeAgentDetailsAuthoringService::ProjectReadableProperties(Snapshot, Properties, Error)) return true;
  MarkNativeReadFailure(Error);
  return false;
}

template<typename T>
bool ReadRecords(UAnimSequenceBase& Asset, const TArray<T>& Records, const TCHAR* Field,
                 FJsonObject& Document, UE::UAI::Contract::FDiagnostic& Error) {
  TArray<TSharedPtr<FJsonValue>> Values;
  UeAgentPropertyValueCodec::FOptions Options;
  Options.StructFieldFilter = [](const UScriptStruct*, const FProperty* Property) { return IsRecordField(Property); };
  for (int32 Index = 0; Index < Records.Num(); ++Index) {
    auto Item = Row(Index, Error);
    if (!Item) return false;
    Item->SetField(TEXT("settings"), UeAgentPropertyValueCodec::EncodeStructValue(
        T::StaticStruct(), &Records[Index], &Asset, Options));
    Values.Add(MakeShared<FJsonValueObject>(Item));
  }
  Document.SetArrayField(Field, Values);
  return true;
}

template<typename T>
bool PrepareRecords(UAnimSequenceBase& Asset, const FString& Field, const TArray<T>& Before, const TSharedPtr<FJsonValue>& Value,
                    TArray<T>& After, UE::UAI::Contract::FDiagnostic& Error, TArray<int32>* CapturedIndices,
                    const FString& CanonicalAssetPath) {
  TArray<HjsonWorkspace::FOrderedStructuredItemBinding> Items;
  if (!Bind(Value, Before.Num(), Items, Error)) {
    UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error, TEXT("/") + Field); return false;
  }
  After.Reset();
  if (CapturedIndices) CapturedIndices->Reset();
  for (const auto& Item : Items) {
    T Record = Item.CapturedIndex == INDEX_NONE ? T() : Before[Item.CapturedIndex];
    TSharedPtr<FJsonValue> Json;
    if (!StateSemantic::CanonicalJson::TryToJsonValue(*Item.Value, Json, Error)) {
      UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error,
          TEXT("/") + Field + TEXT("/") + FString::FromInt(After.Num())); return false;
    }
    if (const auto Settings = Json->AsObject()->TryGetField(TEXT("settings")))
      if (!ApplyFields(T::StaticStruct(), &Record, Asset, Settings, Error, CanonicalAssetPath)) {
        UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error,
            TEXT("/") + Field + TEXT("/") + FString::FromInt(After.Num())); return false;
      }
    if constexpr (std::is_same_v<T, FAnimSyncMarker>) {
      if (Item.CapturedIndex == INDEX_NONE) Record.Guid = FGuid::NewGuid();
    }
    After.Add(MoveTemp(Record));
    if (CapturedIndices) CapturedIndices->Add(Item.CapturedIndex);
  }
  return true;
}
}

bool ReadAnimationEvents(UAnimSequenceBase& Asset, FJsonObject& Document, UE::UAI::Contract::FDiagnostic& Error) {
  if (!ReadRecords(Asset, Asset.AnimNotifyTracks, TEXT("notify_tracks"), Document, Error)) return false;
  if (const auto* Sequence = Cast<UAnimSequence>(&Asset))
    if (!ReadRecords(Asset, Sequence->AuthoredSyncMarkers, TEXT("sync_markers"), Document, Error)) return false;
  TArray<TSharedPtr<FJsonValue>> Values;
  for (int32 Index = 0; Index < Asset.Notifies.Num(); ++Index) {
    const auto& Notify = Asset.Notifies[Index];
    auto Item = Row(Index, Error);
    if (!Item) return false;
    Item->SetNumberField(TEXT("time"), Notify.GetTime());
    Item->SetNumberField(TEXT("duration"), Notify.GetDuration());
    Item->SetNumberField(TEXT("track_index"), Notify.TrackIndex);
    UeAgentAuthoringState::FStateSnapshot Snapshot;
    TSharedPtr<FJsonObject> Properties;
    if (!ReadNotify(Asset, Notify, Snapshot, Properties, Error)) return false;
    Item->SetObjectField(TEXT("settings"), Properties);
    Values.Add(MakeShared<FJsonValueObject>(Item));
  }
  Document.SetArrayField(TEXT("notifies"), Values);
  return true;
}

bool ApplyAnimationEventChanges(UAnimSequence& Asset,
    const UE::UAI::Authoring::Timeline::FSequenceChanges& Changes,
    UE::UAI::Authoring::Timeline::FUeAgentTimelineAnimationExecutionError& OutError, const FString& CanonicalObjectPath) {
  using namespace UE::UAI::Authoring::Timeline;
  using namespace UE::UAI::Authoring::Timeline::Json;
  if (Changes.EventChanges.IsEmpty()) return true;
  FJsonObject Document;
  UE::UAI::Contract::FDiagnostic Error;
  if (!ReadAnimationEvents(Asset, Document, Error)) return Fail(OutError, Error);
  using FSourceLocation = UeAgentAuthoringDiagnostics::FSourceLocation;
  using FSourceScope = UeAgentAuthoringDiagnostics::FScopedSourceLocations;
  const auto InputSources = Changes.SourceRecord ? FSourceScope::CaptureProjection(*Changes.SourceRecord)
      : TMap<FString, FSourceLocation>();
  const auto InputSource = [&](int32 Index, const FString& Field) {
    const FString Pointer = TEXT("/event_changes/") + FString::FromInt(Index) + (Field.IsEmpty() ? FString() : TEXT("/") + Field);
    auto Source = FSourceScope::ResolveProjection(InputSources, Pointer);
    if (Source.JsonPath.IsEmpty()) { Source.JsonPath = Pointer; Source.InternalJsonPath = Pointer; }
    return Source;
  };
  const auto InputFailure = [&](const FString& Code, const FString& Reason, int32 Index,
      const FString& Field, const FString& Expected, const TSharedPtr<FJsonValue>& Actual) {
    auto Diagnostic = UeAgentAuthoringDiagnostics::MakeValueFailure(Code,
        TEXT("/event_changes/") + FString::FromInt(Index) + TEXT("/") + Field,
        MakeShared<FJsonValueString>(Expected), Actual, TEXT("native_execution"), Reason);
    Diagnostic.Source = InputSource(Index, Field);
    Diagnostic.SourceKind = TEXT("related_input");
    Diagnostic.TargetPath = CanonicalObjectPath;
    Diagnostic.SubjectId = Changes.EventChanges[Index].EventRef;
    return Diagnostic;
  };
  struct FRow {
    FName Name; TSharedPtr<FJsonObject> Value; FName Track; int32 SourceIndex = INDEX_NONE;
    TMap<FString, FSourceLocation> Sources;
  };
  const auto BindInput = [&](FRow& Row, const FString& GeneratedPointer, int32 Index, const FString& Field) {
    Row.Sources.Add(GeneratedPointer, InputSource(Index, Field));
  };
  TArray<FRow> Tracks, Notifies, Markers;
  const auto TrackName = [&](int32 Index) {
    return Asset.AnimNotifyTracks.IsValidIndex(Index) ? Asset.AnimNotifyTracks[Index].TrackName : NAME_None;
  };
  for (int32 I = 0; I < Asset.AnimNotifyTracks.Num(); ++I)
    Tracks.Add({Asset.AnimNotifyTracks[I].TrackName, Document.GetArrayField(TEXT("notify_tracks"))[I]->AsObject(), NAME_None});
  for (int32 I = 0; I < Asset.Notifies.Num(); ++I)
    Notifies.Add({Asset.Notifies[I].NotifyName, Document.GetArrayField(TEXT("notifies"))[I]->AsObject(), TrackName(Asset.Notifies[I].TrackIndex), I});
  for (int32 I = 0; I < Asset.AuthoredSyncMarkers.Num(); ++I)
    Markers.Add({Asset.AuthoredSyncMarkers[I].MarkerName, Document.GetArrayField(TEXT("sync_markers"))[I]->AsObject(), TrackName(Asset.AuthoredSyncMarkers[I].TrackIndex)});
  const auto MakeRecord = [](const TCHAR* NameField, const FString& Name) {
    auto Fields = MakeShared<FJsonObject>();
    Fields->SetStringField(NameField, Name);
    auto Settings = MakeShared<FJsonObject>();
    Settings->SetObjectField(TEXT("fields"), Fields);
    auto Row = MakeShared<FJsonObject>();
    Row->SetObjectField(TEXT("settings"), Settings);
    return Row;
  };
  const auto FindRecord = [&](const TArray<FRow>& Rows, const FString& Ref, int32 ChangeIndex, int32& Index) {
    Index = Rows.IndexOfByPredicate([&](const FRow& R) {
      FString Instance;
      return R.Value->TryGetStringField(TEXT("instance"), Instance) && Instance == Ref;
    });
    if (Index != INDEX_NONE) return true;
    for (int32 I = 0; I < Rows.Num(); ++I) {
      if (Rows[I].Name != FName(*Ref)) continue;
      if (Index != INDEX_NONE) {
        auto Diagnostic = InputFailure(TEXT("animation_event_ref_ambiguous"),
            TEXT("The event reference matches more than one record; select its unique instance key."), ChangeIndex,
            TEXT("event_ref"), TEXT("unique event instance key"), MakeShared<FJsonValueString>(Ref));
        const auto Actual = MakeShared<FJsonObject>();
        Actual->SetStringField(TEXT("reference"), Ref);
        Actual->SetArrayField(TEXT("matching_indices"), {MakeShared<FJsonValueNumber>(Index), MakeShared<FJsonValueNumber>(I)});
        Diagnostic.ActualValue = MakeShared<FJsonValueObject>(Actual);
        return Fail(OutError, MoveTemp(Diagnostic));
      }
      Index = I;
    }
    return true;
  };
  // Resolve track identity once, after all track edits; surviving events keep their named owner.
  for (int32 ChangeIndex = 0; ChangeIndex < Changes.EventChanges.Num(); ++ChangeIndex) {
    const auto& Change = Changes.EventChanges[ChangeIndex];
    if (Change.EventKind != TEXT("notify_track")) continue;
    const FName Name(*Change.EventRef);
    int32 Index = INDEX_NONE;
    if (!FindRecord(Tracks, Change.EventRef, ChangeIndex, Index)) return false;
    if (Change.Operation == TEXT("remove")) {
      if (Index != INDEX_NONE) {
        const FName RemovedName = Tracks[Index].Name;
        Tracks.RemoveAt(Index);
        const FName Fallback = Tracks.IsEmpty() ? NAME_None : Tracks[FMath::Max(0, Index - 1)].Name;
        for (auto* Rows : {&Notifies, &Markers})
          for (auto& Row : *Rows) if (Row.Track == RemovedName) {
            Row.Track = Fallback;
            BindInput(Row, Rows == &Markers ? TEXT("/settings/fields/TrackIndex") : TEXT("/track_index"), ChangeIndex, TEXT("event_ref"));
          }
      }
    }
    else if (Index == INDEX_NONE) {
      auto& Row = Tracks.Add_GetRef({Name, MakeRecord(TEXT("TrackName"), Change.EventRef), NAME_None});
      BindInput(Row, TEXT("/"), ChangeIndex, TEXT("event_ref"));
    }
  }
  const FString Clock = TEXT("animation_sequence:") + (CanonicalObjectPath.IsEmpty() ? Asset.GetPathName() : CanonicalObjectPath);
  const FFrameRate Rate = Asset.GetDataModel()->GetFrameRate();
  const auto Seconds = [&](const FTimelineAnimationPoint& Point, double& Value) {
    int64 Tick = 0;
    return TryPointTick(Point, Clock, Rate, Tick, OutError) && TryPointSeconds(Point, Value, OutError);
  };
  const auto SetNotifyValue = [&](FJsonObject& Settings, const TCHAR* Name, const TSharedPtr<FJsonValue>& Value, const FString& EventRef) {
    for (const auto& Category : Settings.Values) {
      const auto Object = Category.Value->AsObject();
      const TSharedPtr<FJsonObject>* Row = nullptr;
      if (Object && Object->TryGetObjectField(Name, Row)) { (*Row)->SetField(TEXT("value"), Value); return true; }
    }
    auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_notify_property_unavailable"),
        FString::Printf(TEXT("Notify '%s' does not expose the native property required for this edit."), *EventRef), TEXT("native_execution"));
    Diagnostic.SourceKind = TEXT("native_result");
    Diagnostic.TargetPath = CanonicalObjectPath;
    Diagnostic.SubjectId = EventRef;
    Diagnostic.PropertyPath = Name;
    Diagnostic.ActualValue = MakeShared<FJsonValueNull>();
    Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("writable notify property"));
    return Fail(OutError, MoveTemp(Diagnostic));
  };
  for (int32 ChangeIndex = 0; ChangeIndex < Changes.EventChanges.Num(); ++ChangeIndex) {
    const auto& Change = Changes.EventChanges[ChangeIndex];
    if (Change.EventKind == TEXT("notify_track")) continue;
    const bool bMarker = Change.EventKind == TEXT("sync_marker");
    auto& Rows = bMarker ? Markers : Notifies;
    const FName Name(*Change.EventRef);
    int32 Index = INDEX_NONE;
    if (!FindRecord(Rows, Change.EventRef, ChangeIndex, Index)) return false;
    if (Change.Operation == TEXT("remove")) { if (Index != INDEX_NONE) Rows.RemoveAt(Index); continue; }
    double Time = 0.0;
    if (!Change.Time.IsSet()) return Fail(OutError, InputFailure(TEXT("animation_event_time_missing"),
        FString::Printf(TEXT("Event '%s' requires a time value for add or update."), *Change.EventRef),
        ChangeIndex, TEXT("time"), TEXT("timeline point"), nullptr));
    if (!Seconds(*Change.Time, Time)) return PrefixChildFailure(Changes.SourceRecord,
        FString::Printf(TEXT("/event_changes/%d/time"), ChangeIndex), OutError);
    if (Index == INDEX_NONE) {
      TSharedPtr<FJsonObject> Row;
      if (bMarker) Row = MakeRecord(TEXT("MarkerName"), Change.EventRef);
      else {
        Row = MakeShared<FJsonObject>();
        UeAgentAuthoringState::FStateSnapshot Snapshot;
        TSharedPtr<FJsonObject> Settings;
        if (!ReadNotify(Asset, FAnimNotifyEvent(), Snapshot, Settings, Error)) return Fail(OutError, Error);
        // A new event carries only fields supplied by this compatibility input.
        // A captured default is not an explicit user choice (notably the class's
        // trigger threshold); let ApplyAnimationEvents initialize those defaults.
        for (auto& Category : Settings->Values) {
          const auto Object = Category.Value->AsObject();
          if (!Object) continue;
          for (auto It = Object->Values.CreateIterator(); It; ++It) {
            if (It.Key() != TEXT("NotifyName") && It.Key() != TEXT("Notify") &&
                It.Key() != TEXT("NotifyStateClass")) It.RemoveCurrent();
          }
        }
        if (!SetNotifyValue(*Settings, TEXT("NotifyName"), MakeShared<FJsonValueString>(Change.EventRef), Change.EventRef)) return false;
        Row->SetObjectField(TEXT("settings"), Settings);
      }
      Index = Rows.Add({Name, Row, Tracks.IsEmpty() ? NAME_None : Tracks[0].Name});
    }
    auto& Row = Rows[Index];
    BindInput(Row, TEXT("/"), ChangeIndex, TEXT("event_ref"));
    if (!Change.TrackRef.IsEmpty()) {
      Row.Track = FName(*Change.TrackRef);
      BindInput(Row, bMarker ? TEXT("/settings/fields/TrackIndex") : TEXT("/track_index"), ChangeIndex, TEXT("track_ref"));
    }
    if (bMarker) {
      Row.Value->GetObjectField(TEXT("settings"))->GetObjectField(TEXT("fields"))->SetNumberField(TEXT("Time"), Time);
      BindInput(Row, TEXT("/settings/fields/Time"), ChangeIndex, TEXT("time"));
    } else {
      Row.Value->SetNumberField(TEXT("time"), Time);
      BindInput(Row, TEXT("/time"), ChangeIndex, TEXT("time"));
      if (Change.Range.IsSet()) {
        double Lower = 0.0, Upper = 0.0;
        if (!Change.Range->Lower.Point.IsSet() || !Change.Range->Upper.Point.IsSet()) {
          const auto Actual = MakeShared<FJsonObject>();
          Actual->SetBoolField(TEXT("lower_point_present"), Change.Range->Lower.Point.IsSet());
          Actual->SetBoolField(TEXT("upper_point_present"), Change.Range->Upper.Point.IsSet());
          return Fail(OutError, InputFailure(TEXT("animation_notify_range_requires_bounds"),
              FString::Printf(TEXT("Notify '%s' requires both lower and upper duration range points."), *Change.EventRef), ChangeIndex, TEXT("range"),
              TEXT("range with lower and upper points"), MakeShared<FJsonValueObject>(Actual)));
        }
        if (!Seconds(*Change.Range->Lower.Point, Lower)) return PrefixChildFailure(Changes.SourceRecord,
            FString::Printf(TEXT("/event_changes/%d/range/lower"), ChangeIndex), OutError);
        if (!Seconds(*Change.Range->Upper.Point, Upper)) return PrefixChildFailure(Changes.SourceRecord,
            FString::Printf(TEXT("/event_changes/%d/range/upper"), ChangeIndex), OutError);
        if (Upper < Lower || !FMath::IsNearlyEqual(Lower, Time)) {
          const auto Actual = MakeShared<FJsonObject>();
          Actual->SetNumberField(TEXT("lower_seconds"), Lower);
          Actual->SetNumberField(TEXT("upper_seconds"), Upper);
          Actual->SetNumberField(TEXT("event_seconds"), Time);
          return Fail(OutError, InputFailure(TEXT("animation_notify_range_invalid"),
              FString::Printf(TEXT("Notify '%s' requires a range starting at its event time and ending at or after its start."), *Change.EventRef), ChangeIndex,
              TEXT("range"), TEXT("lower equals event time; upper >= lower"), MakeShared<FJsonValueObject>(Actual)));
        }
        Row.Value->SetNumberField(TEXT("duration"), Upper - Lower);
        BindInput(Row, TEXT("/duration"), ChangeIndex, TEXT("range"));
      }
      if (!Change.PayloadClassRef.IsEmpty()) {
        const auto* Original = Asset.Notifies.IsValidIndex(Row.SourceIndex)
            ? &Asset.Notifies[Row.SourceIndex] : nullptr;
        const bool bState = Change.EventKind == TEXT("notify_state");
        const UObject* ExistingPayload = Original
            ? (bState ? static_cast<const UObject*>(Original->NotifyStateClass.Get()) : static_cast<const UObject*>(Original->Notify.Get()))
            : nullptr;
        // A class replacement creates a new owned notify; it must not ask the
        // property owner to change the class of an existing UObject in place.
        if (!ExistingPayload || ExistingPayload->GetClass()->GetPathName() != Change.PayloadClassRef)
          Row.Value->RemoveField(TEXT("instance"));
        auto Owned = MakeShared<FJsonObject>();
        Owned->SetStringField(TEXT("class_path"), Change.PayloadClassRef);
        auto Value = MakeShared<FJsonObject>();
        Value->SetObjectField(TEXT("owned_object"), Owned);
        auto Settings = Row.Value->GetObjectField(TEXT("settings"));
        if (!SetNotifyValue(*Settings, bState ? TEXT("NotifyStateClass") : TEXT("Notify"), MakeShared<FJsonValueObject>(Value), Change.EventRef) ||
            !SetNotifyValue(*Settings, bState ? TEXT("Notify") : TEXT("NotifyStateClass"), MakeShared<FJsonValueNull>(), Change.EventRef)) return false;
        BindInput(Row, TEXT("/settings"), ChangeIndex, TEXT("payload_class_ref"));
      }
    }
  }
  TMap<FName, int32> TrackIndices;
  TrackIndices.Reserve(Tracks.Num());
  for (int32 Index = 0; Index < Tracks.Num(); ++Index) {
    if (!TrackIndices.Contains(Tracks[Index].Name)) TrackIndices.Add(Tracks[Index].Name, Index);
  }
  for (auto* Rows : {&Notifies, &Markers}) {
    for (auto& Row : *Rows) {
      const int32* FoundTrack = TrackIndices.Find(Row.Track);
      const int32 Track = FoundTrack ? *FoundTrack : INDEX_NONE;
      if (Track == INDEX_NONE) {
        auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_event_track_missing"),
            FString::Printf(TEXT("Event '%s' refers to a notify track that does not exist after the track edits."), *Row.Name.ToString()), TEXT("native_execution"));
        Diagnostic.SubjectId = Row.Name.ToString();
        Diagnostic.TargetPath = CanonicalObjectPath;
        Diagnostic.ActualValue = MakeShared<FJsonValueString>(Row.Track.ToString());
        Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("existing notify track name"));
        const auto Source = FSourceScope::ResolveProjection(Row.Sources,
            Rows == &Markers ? TEXT("/settings/fields/TrackIndex") : TEXT("/track_index"));
        if (!Source.JsonPath.IsEmpty()) { Diagnostic.Source = Source; Diagnostic.SourceKind = TEXT("related_input"); }
        else Diagnostic.SourceKind = TEXT("native_result");
        return Fail(OutError, MoveTemp(Diagnostic));
      }
      if (Rows == &Markers) Row.Value->GetObjectField(TEXT("settings"))->GetObjectField(TEXT("fields"))->SetNumberField(TEXT("TrackIndex"), Track);
      else Row.Value->SetNumberField(TEXT("track_index"), Track);
    }
  }
  TMap<FString, FSourceLocation> GeneratedSources;
  const auto PublishRows = [&](const TCHAR* Field, const TArray<FRow>& Rows) {
    TArray<TSharedPtr<FJsonValue>> Values;
    for (const auto& Row : Rows) {
      const FString Base = TEXT("/") + FString(Field) + TEXT("/") + FString::FromInt(Values.Num());
      for (const auto& Source : Row.Sources)
        GeneratedSources.Add(Base + (Source.Key == TEXT("/") ? FString() : Source.Key), Source.Value);
      Values.Add(MakeShared<FJsonValueObject>(Row.Value));
    }
    Document.SetArrayField(Field, Values);
  };
  PublishRows(TEXT("notify_tracks"), Tracks);
  PublishRows(TEXT("notifies"), Notifies);
  PublishRows(TEXT("sync_markers"), Markers);
  int32 Applied = 0;
  if (ApplyAnimationEvents(Asset, Document, Applied, Error, true, nullptr, CanonicalObjectPath)) return true;
  TFunction<void(UE::UAI::Contract::FDiagnostic&)> BindGeneratedFailure = [&](UE::UAI::Contract::FDiagnostic& Diagnostic) {
    for (auto& Child : Diagnostic.Diagnostics) BindGeneratedFailure(Child);
    if (Diagnostic.PublicCause) {
      auto Cause = MakeShared<UE::UAI::Contract::FDiagnostic>(*Diagnostic.PublicCause);
      BindGeneratedFailure(*Cause); Diagnostic.PublicCause = MoveTemp(Cause);
    }
    if (Diagnostic.SourceKind == TEXT("native_result") || Diagnostic.SourceKind == TEXT("command_input")
        || !Diagnostic.Source.SourceFile.IsEmpty()
        || (!Diagnostic.Source.SourceUnit.IsEmpty() && !Diagnostic.Source.InternalJsonPath.IsEmpty())) return;
    const FString Pointer = !Diagnostic.Source.InternalJsonPath.IsEmpty() ? Diagnostic.Source.InternalJsonPath
        : !Diagnostic.SemanticPointer.IsEmpty() ? Diagnostic.SemanticPointer : Diagnostic.Source.JsonPath;
    const auto Source = FSourceScope::ResolveProjection(GeneratedSources, Pointer);
    if (!Source.JsonPath.IsEmpty()) { Diagnostic.Source = Source; Diagnostic.SourceKind = TEXT("related_input"); }
    else { Diagnostic.Source = {}; Diagnostic.SourceKind = TEXT("native_result"); }
  };
  BindGeneratedFailure(Error);
  if (Error.TargetPath.IsEmpty()) Error.TargetPath = CanonicalObjectPath;
  return Fail(OutError, MoveTemp(Error));
}

bool ApplyAnimationEvents(UAnimSequenceBase& Asset, const FJsonObject& Document, int32& Changes, UE::UAI::Contract::FDiagnostic& Error,
    bool bTrackIndicesResolved, const TArray<FAnimNotifyEvent>* CapturedNotifies, const FString& CanonicalAssetPath) {
  auto Tracks = Asset.AnimNotifyTracks;
  auto Notifies = Asset.Notifies;
  auto* Sequence = Cast<UAnimSequence>(&Asset);
  TArray<FAnimSyncMarker> Markers = Sequence ? Sequence->AuthoredSyncMarkers : TArray<FAnimSyncMarker>();
  TArray<int32> TrackSources, MarkerSources, NotifySources;
  for (int32 Index = 0; Index < Tracks.Num(); ++Index) TrackSources.Add(Index);
  for (int32 Index = 0; Index < Markers.Num(); ++Index) MarkerSources.Add(Index);
  for (int32 Index = 0; Index < Notifies.Num(); ++Index) NotifySources.Add(Index);
  bool bChanged = false;
  if (const auto Value = Document.TryGetField(TEXT("notify_tracks"))) {
    if (!PrepareRecords(Asset, TEXT("notify_tracks"), Asset.AnimNotifyTracks, Value, Tracks, Error, &TrackSources, CanonicalAssetPath)) return false;
    bChanged = true;
  }
  if (Sequence) {
    if (const auto Value = Document.TryGetField(TEXT("sync_markers"))) {
      if (!PrepareRecords(Asset, TEXT("sync_markers"), Sequence->AuthoredSyncMarkers, Value, Markers, Error, &MarkerSources, CanonicalAssetPath)) return false;
      bChanged = true;
    }
  }
  if (const auto Value = Document.TryGetField(TEXT("notifies"))) {
    TArray<HjsonWorkspace::FOrderedStructuredItemBinding> Items;
    if (!Bind(Value, Asset.Notifies.Num(), Items, Error)) {
      UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error, TEXT("/notifies")); return false;
    }
    Notifies.Reset();
    NotifySources.Reset();
    for (const auto& Item : Items) {
      FAnimNotifyEvent Notify = Item.CapturedIndex == INDEX_NONE ? FAnimNotifyEvent() : Asset.Notifies[Item.CapturedIndex];
      const auto* PreviousNotify = Notify.Notify.Get();
      const auto* PreviousState = Notify.NotifyStateClass.Get();
      bool bExplicitThreshold = false;
      if (Item.CapturedIndex == INDEX_NONE) Notify.Guid = FGuid::NewGuid();
      TSharedPtr<FJsonValue> Json;
      if (!StateSemantic::CanonicalJson::TryToJsonValue(*Item.Value, Json, Error)) {
        UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error,
            TEXT("/notifies/") + FString::FromInt(Notifies.Num())); return false;
      }
      const auto RowObject = Json->AsObject();
      if (const auto Settings = RowObject->TryGetField(TEXT("settings"))) {
        UeAgentAuthoringState::FStateSnapshot Snapshot;
        TSharedPtr<FJsonObject> Properties;
        TArray<TSharedPtr<FJsonValue>> Descriptors;
        if (!ReadNotify(Asset, Notify, Snapshot, Properties, Error)) return false;
        if (!UeAgentDetailsAuthoringService::DecodeReadableProperties(Snapshot, Settings, Descriptors, Error)) {
          UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error,
              TEXT("/notifies/") + FString::FromInt(Notifies.Num()) + TEXT("/settings")); return false;
        }
        UeAgentAuthoringObjectApply::FOptions Options;
        Options.DiagnosticTargetPath = CanonicalAssetPath;
        // Existing notify objects also belong to the domain's proposed asset
        // during preflight; carry the same owner policy as the root provider.
        Options.bAllowDomainAuthorizedOwners = true;
        Options.bCompactValues = true;
        Options.bEmitEditorNotifications = false;
        Options.bMarkPackageDirty = false;
        Options.StructNotificationOwnerPropertyPath = TEXT("Notifies");
        for (const auto& Descriptor : Descriptors) {
          FString Path;
          if (Descriptor->AsObject()->TryGetStringField(TEXT("property_path"), Path) && Path == TEXT("TriggerWeightThreshold"))
            bExplicitThreshold = true;
        }
        UeAgentAuthoringObjectApply::FPreparedApply Prepared;
        UeAgentAuthoringObjectApply::FFailure Failure;
        UeAgentAuthoringObjectApply::FApplyResult Applied;
        if (!UeAgentAuthoringObjectApply::PrepareStruct(FAnimNotifyEvent::StaticStruct(), &Notify, &Asset,
            Descriptors, Options, Prepared, Failure)) {
          Error = Failure.Diagnostic;
          UeAgentDetailsAuthoringService::LocateReadablePropertyFailure(Error, Settings,
              TEXT("/notifies/") + FString::FromInt(Notifies.Num()) + TEXT("/settings")); return false;
        }
        if (!UeAgentAuthoringObjectApply::ApplyPrepared(Prepared, Applied)) {
          Error = Applied.Diagnostic;
          UeAgentDetailsAuthoringService::LocateReadablePropertyFailure(Error, Settings,
              TEXT("/notifies/") + FString::FromInt(Notifies.Num()) + TEXT("/settings")); return false;
        }
      }
      double Time = Notify.GetTime(), Duration = Notify.GetDuration();
      const bool bNewNotify = Notify.Notify && Notify.Notify != PreviousNotify;
      const bool bNewState = Notify.NotifyStateClass && Notify.NotifyStateClass != PreviousState;
      if (bNewState && !RowObject->HasField(TEXT("duration"))) Duration = 1.0 / 30.0;
      if (!bExplicitThreshold) {
        if (bNewState) Notify.TriggerWeightThreshold = Notify.NotifyStateClass->GetDefaultTriggerWeightThreshold();
        else if (bNewNotify) Notify.TriggerWeightThreshold = Notify.Notify->GetDefaultTriggerWeightThreshold();
      }
      const bool bTimeValid = !RowObject->HasField(TEXT("time")) || RowObject->TryGetNumberField(TEXT("time"), Time);
      const bool bDurationValid = bTimeValid && (!RowObject->HasField(TEXT("duration")) || RowObject->TryGetNumberField(TEXT("duration"), Duration));
      if (!bTimeValid || !bDurationValid) {
        const FString Field = !bTimeValid ? TEXT("time") : TEXT("duration");
        Error = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("animation_notify_time_or_track_expected_number"),
            TEXT("/notifies/") + FString::FromInt(Notifies.Num()) + TEXT("/") + Field,
            MakeShared<FJsonValueString>(TEXT("number")), RowObject->TryGetField(Field), TEXT("authoring"),
            TEXT("The notify ") + Field + TEXT(" must be a number."));
        UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *RowObject, Field);
        return false;
      }
      if (!UeAgentAuthoringJsonFields::ReadInt32Field(*RowObject, TEXT("track_index"), Notify.TrackIndex, Error)) {
        UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error,
            TEXT("/notifies/") + FString::FromInt(Notifies.Num()) + TEXT("/track_index")); return false;
      }
      if (!FMath::IsFinite(Time) || !FMath::IsFinite(Duration) || Duration < 0.0) {
        const FString Field = !FMath::IsFinite(Time) ? TEXT("time") : TEXT("duration");
        const double NumericValue = Field == TEXT("time") ? Time : Duration;
        Error = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("animation_notify_time_invalid"),
            TEXT("/notifies/") + FString::FromInt(Notifies.Num()) + TEXT("/") + Field,
            MakeShared<FJsonValueString>(Field == TEXT("time") ? TEXT("finite number") : TEXT("finite number >= 0")),
            FMath::IsFinite(NumericValue) ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(NumericValue))
                : MakeShared<FJsonValueString>(LexToString(NumericValue)), TEXT("authoring"),
            Field == TEXT("time") ? TEXT("The notify time must be finite.") : TEXT("The notify duration must be finite and non-negative."));
        if (RowObject->HasField(Field)) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *RowObject, Field);
        else Error.SourceKind = TEXT("native_result");
        return false;
      }
      float NativeTime = 0, NativeDuration = 0, NativeEnd = 0;
      const bool bTimeConverted = UeAgentPropertyValueCodec::TryConvertFloat32(Time, NativeTime, Error);
      const bool bDurationConverted = bTimeConverted && UeAgentPropertyValueCodec::TryConvertFloat32(Duration, NativeDuration, Error);
      const bool bEndConverted = bDurationConverted && UeAgentPropertyValueCodec::TryConvertFloat32(Time + Duration, NativeEnd, Error);
      if (!bEndConverted) {
        const FString Field = !bTimeConverted ? TEXT("time") : !bDurationConverted ? TEXT("duration") : FString();
        UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error,
            TEXT("/notifies/") + FString::FromInt(Notifies.Num()) + (Field.IsEmpty() ? FString() : TEXT("/") + Field));
        if (Field.IsEmpty()) {
          Error.PropertyPath = TEXT("time + duration");
          if (RowObject->HasField(TEXT("time")) || RowObject->HasField(TEXT("duration"))) {
            UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, Document);
            Error.SourceKind = TEXT("related_input");
          } else Error.SourceKind = TEXT("native_result");
        } else if (RowObject->HasField(Field)) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *RowObject, Field);
        else Error.SourceKind = TEXT("native_result");
        return false;
      }
      // Montage structure edits have already updated native linkable elements.
      // Captured times in an otherwise unchanged row must not undo that update.
      const FAnimNotifyEvent* Captured = CapturedNotifies && CapturedNotifies->IsValidIndex(Item.CapturedIndex)
          ? &(*CapturedNotifies)[Item.CapturedIndex] : nullptr;
      const bool bTimeEdited = !Captured || (RowObject->HasField(TEXT("time")) && NativeTime != Captured->GetTime());
      const bool bDurationEdited = !Captured || (RowObject->HasField(TEXT("duration")) && NativeDuration != Captured->GetDuration());
      if (!bTimeEdited) NativeTime = Notify.GetTime();
      if (!bDurationEdited) NativeDuration = Notify.GetDuration();
      NativeEnd = NativeTime + NativeDuration;
      if (bTimeEdited) Notify.Link(&Asset, NativeTime, Notify.GetSlotIndex());
      if (bDurationEdited) Notify.SetDuration(NativeDuration);
      if (Notify.NotifyStateClass && (bTimeEdited || bDurationEdited || bNewState))
        Notify.EndLink.Link(&Asset, NativeEnd, Notify.GetSlotIndex());
      Notify.RefreshTriggerOffset(Asset.CalculateOffsetForNotify(NativeTime));
      Notify.RefreshEndTriggerOffset(Asset.CalculateOffsetForNotify(NativeEnd));
      if (bNewNotify) Notify.Notify->OnAnimNotifyCreatedInEditor(Notify);
      else if (bNewState) Notify.NotifyStateClass->OnAnimNotifyCreatedInEditor(Notify);
      Notifies.Add(MoveTemp(Notify));
      NotifySources.Add(Item.CapturedIndex);
    }
    bChanged = true;
  }
  // TrackIndex is an editor relationship, not the identity of a track. Preserve
  // unchanged relationships through insert/reorder; removal follows Persona's
  // previous-track fallback. A changed index is an explicit desired index.
  const auto RemapUnchangedTrack = [&](int32 BeforeIndex, int32& DesiredIndex) {
    if (bTrackIndicesResolved || DesiredIndex != BeforeIndex || !Asset.AnimNotifyTracks.IsValidIndex(BeforeIndex)) return;
    int32 Source = BeforeIndex;
    int32 Mapped = TrackSources.IndexOfByKey(Source);
    while (Mapped == INDEX_NONE && --Source >= 0) Mapped = TrackSources.IndexOfByKey(Source);
    DesiredIndex = Mapped != INDEX_NONE ? Mapped : (Tracks.IsEmpty() ? INDEX_NONE : 0);
  };
  for (int32 Index = 0; Index < Notifies.Num(); ++Index)
    if (Asset.Notifies.IsValidIndex(NotifySources[Index]))
      RemapUnchangedTrack(Asset.Notifies[NotifySources[Index]].TrackIndex, Notifies[Index].TrackIndex);
  if (Sequence)
    for (int32 Index = 0; Index < Markers.Num(); ++Index)
      if (Sequence->AuthoredSyncMarkers.IsValidIndex(MarkerSources[Index]))
        RemapUnchangedTrack(Sequence->AuthoredSyncMarkers[MarkerSources[Index]].TrackIndex, Markers[Index].TrackIndex);
  // RefreshCacheData indexes these editor tracks; reject dangling explicit
  // indices (including an attempted removal of every still-used track).
  for (int32 Index = 0; Index < Notifies.Num(); ++Index) {
    const auto& Notify = Notifies[Index];
    if (!Tracks.IsValidIndex(Notify.TrackIndex)) {
      Error = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("animation_notify_track_missing"),
          TEXT("/notifies/") + FString::FromInt(Index) + TEXT("/track_index"),
          MakeShared<FJsonValueString>(FString::Printf(TEXT("notify track index in [0, %d)"), Tracks.Num())),
          MakeShared<FJsonValueNumber>(Notify.TrackIndex), TEXT("authoring"),
          FString::Printf(TEXT("Notify '%s' refers to a track that does not exist."), *Notify.NotifyName.ToString()));
      Error.SubjectId = Notify.NotifyName.ToString();
      if (Document.HasField(TEXT("notifies"))) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, Document);
      else {
        Error.SourceKind = Document.HasField(TEXT("notify_tracks")) ? TEXT("related_input") : TEXT("native_result");
        Error.Source.JsonPath = TEXT("/notify_tracks");
        Error.Source.InternalJsonPath = TEXT("/notify_tracks");
      }
      return false;
    }
  }
  for (int32 Index = 0; Index < Markers.Num(); ++Index) {
    const auto& Marker = Markers[Index];
    if (!Tracks.IsValidIndex(Marker.TrackIndex) || !FMath::IsFinite(Marker.Time)) {
      const bool bTrackInvalid = !Tracks.IsValidIndex(Marker.TrackIndex);
      Error = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("animation_sync_marker_track_or_time_invalid"),
          TEXT("/sync_markers/") + FString::FromInt(Index) + (bTrackInvalid ? TEXT("/settings/fields/TrackIndex") : TEXT("/settings/fields/Time")),
          MakeShared<FJsonValueString>(bTrackInvalid ? FString::Printf(TEXT("notify track index in [0, %d)"), Tracks.Num()) : TEXT("finite time")),
          bTrackInvalid ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(Marker.TrackIndex)) : MakeShared<FJsonValueString>(LexToString(Marker.Time)),
          TEXT("authoring"), bTrackInvalid ? FString::Printf(TEXT("Sync marker '%s' refers to a track that does not exist."), *Marker.MarkerName.ToString())
              : FString::Printf(TEXT("Sync marker '%s' requires a finite time."), *Marker.MarkerName.ToString()));
      Error.SubjectId = Marker.MarkerName.ToString();
      if (Document.HasField(TEXT("sync_markers"))) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, Document);
      else {
        Error.SourceKind = bTrackInvalid && Document.HasField(TEXT("notify_tracks")) ? TEXT("related_input") : TEXT("native_result");
        Error.Source.JsonPath = TEXT("/notify_tracks");
        Error.Source.InternalJsonPath = TEXT("/notify_tracks");
      }
      return false;
    }
  }
  if (bChanged) {
    Asset.AnimNotifyTracks = MoveTemp(Tracks);
    Asset.Notifies = MoveTemp(Notifies);
    if (Sequence) Sequence->AuthoredSyncMarkers = MoveTemp(Markers);
    Asset.RefreshCacheData();
    ++Changes;
  }
  return true;
}
}
