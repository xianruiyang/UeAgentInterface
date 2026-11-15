#include "Domains/Animation/Authoring/UeAgentAnimationMontageStructure.h"

#include "Animation/AnimMontage.h"
#include "Animation/AnimCompositeBase.h"
#include "Core/HjsonWorkspace/UeAgentKeyedItemAuthoringProjection.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Domains/Animation/Authoring/UeAgentAnimationEvents.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Factories/AnimMontageFactory.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringObjectMetadata.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticCompare.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"
#include "Utilities/AuthoringCore/UeAgentNativeNormalizationIntent.h"
#include "Utilities/AuthoringObjectApply/UeAgentAuthoringObjectApply.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "UObject/UnrealType.h"

namespace UeAgentAnimationAuthoring {
namespace {
using namespace UE::UAI;

bool IsField(const FProperty* Property) {
  if (!Property) return false;
  const UStruct* Owner = Property->GetOwnerStruct();
  const FName Name = Property->GetFName();
  if (Owner == FAnimSegment::StaticStruct()) return Name != TEXT("CachedPlayLength") && Name != TEXT("StartPos");
  if (Owner == FSlotAnimationTrack::StaticStruct()) return Name == TEXT("SlotName");
  if (Owner == FCompositeSection::StaticStruct())
    return Name == TEXT("SectionName");
  return false;
}

bool ReadSectionProperties(UAnimMontage& Asset, const FCompositeSection& Section,
    UeAgentAuthoringState::FStateSnapshot& Snapshot, TSharedPtr<FJsonObject>& Properties, UE::UAI::Contract::FDiagnostic& Error) {
  UeAgentDetailsAuthoringService::FCapturePolicy Policy;
  Policy.IncludeProperty = [](const FProperty* Property, const void*, const UObject*) {
    return Property && Property->GetFName() == TEXT("MetaData");
  };
  return UeAgentDetailsAuthoringService::CaptureStructProperties(FCompositeSection::StaticStruct(), &Section,
      Asset.GetPathName(), &Asset, Policy, Snapshot, Properties, Error) &&
      UeAgentDetailsAuthoringService::ProjectReadableProperties(Snapshot, Properties, Error);
}

FString SectionBindingKey(int32 SourceIndex, int32 DesiredIndex) {
  return SourceIndex == INDEX_NONE ? TEXT("new:") + FString::FromInt(DesiredIndex) : FString::FromInt(SourceIndex);
}

bool ApplySectionProperties(UAnimMontage& Asset, FCompositeSection& Section,
    const TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& Error, const FString& CanonicalAssetPath,
    TSharedPtr<FJsonObject>* BoundRows) {
  UeAgentAuthoringState::FStateSnapshot Snapshot;
  TSharedPtr<FJsonObject> Properties;
  TArray<TSharedPtr<FJsonValue>> Descriptors;
  if (!ReadSectionProperties(Asset, Section, Snapshot, Properties, Error) ||
      !UeAgentDetailsAuthoringService::DecodeReadableProperties(Snapshot, Value, Descriptors, Error, BoundRows)) return false;
  UeAgentAuthoringObjectApply::FOptions Options;
  Options.DiagnosticTargetPath = CanonicalAssetPath;
  Options.bAllowDomainAuthorizedOwners = true;
  Options.bCompactValues = true;
  Options.bEmitEditorNotifications = false;
  Options.bMarkPackageDirty = false;
  Options.StructNotificationOwnerPropertyPath = TEXT("CompositeSections");
  UeAgentAuthoringObjectApply::FPreparedApply Prepared;
  UeAgentAuthoringObjectApply::FFailure Failure;
  UeAgentAuthoringObjectApply::FApplyResult Applied;
  if (!UeAgentAuthoringObjectApply::PrepareStruct(FCompositeSection::StaticStruct(), &Section, &Asset,
      Descriptors, Options, Prepared, Failure)) {
    Error = Failure.Diagnostic;
    UeAgentDetailsAuthoringService::LocateReadablePropertyFailure(Error, Value, TEXT("/properties")); return false;
  }
  if (!UeAgentAuthoringObjectApply::ApplyPrepared(Prepared, Applied)) {
    Error = Applied.Diagnostic;
    UeAgentDetailsAuthoringService::LocateReadablePropertyFailure(Error, Value, TEXT("/properties")); return false;
  }
  return true;
}

template<typename T>
TSharedPtr<FJsonValue> ReadSettings(const T& Record, UAnimMontage& Asset) {
  UeAgentPropertyValueCodec::FOptions Options;
  Options.StructFieldFilter = [](const UScriptStruct*, const FProperty* Property) { return IsField(Property); };
  return UeAgentPropertyValueCodec::EncodeStructValue(T::StaticStruct(), &Record, &Asset, Options);
}

template<typename T>
bool ApplySettings(T& Record, UAnimMontage& Asset, const FJsonObject& Row, UE::UAI::Contract::FDiagnostic& Error,
    const FString& CanonicalAssetPath) {
  const auto Settings = Row.TryGetField(TEXT("settings"));
  if (!Settings) return true;
  const TSharedPtr<FJsonObject>* Object = nullptr;
  const TSharedPtr<FJsonObject>* Fields = nullptr;
  if (!Settings->TryGetObject(Object) || !Object || !(*Object)->TryGetObjectField(TEXT("fields"), Fields) || !Fields) {
    const bool bObject = Object && Object->IsValid();
    Error = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("animation_montage_settings_fields_expected"),
        bObject ? TEXT("/settings/fields") : TEXT("/settings"), MakeShared<FJsonValueString>(TEXT("object")),
        bObject ? (*Object)->TryGetField(TEXT("fields")) : Settings, TEXT("original_intent"),
        bObject ? TEXT("Montage record settings require a fields object.") : TEXT("Montage record settings must be an object."));
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
  Options.bAllowDomainAuthorizedOwners = true;
  Options.bEmitEditorNotifications = false;
  Options.bMarkPackageDirty = false;
  Options.AuthorizeProperty = IsField;
  Options.IncludeProperty = IsField;
  UeAgentAuthoringObjectApply::FPreparedApply Prepared;
  UeAgentAuthoringObjectApply::FFailure Failure;
  UeAgentAuthoringObjectApply::FApplyResult Applied;
  if (!UeAgentAuthoringObjectApply::PrepareStruct(T::StaticStruct(), &Record, &Asset, Descriptors, Options, Prepared, Failure)) {
    auto Diagnostic = Failure.Diagnostic;
    Error = Diagnostic;
    UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error, TEXT("/settings/fields"), true); return false;
  }
  if (!UeAgentAuthoringObjectApply::ApplyPrepared(Prepared, Applied)) {
    Error = Applied.Diagnostic;
    UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error, TEXT("/settings/fields"), true); return false;
  }
  return true;
}

template<typename T, typename F>
bool ReadRows(UAnimMontage& Asset, const TArray<T>& Records, TArray<TSharedPtr<FJsonValue>>& Rows,
              F&& Extra, UE::UAI::Contract::FDiagnostic& Error) {
  for (int32 Index = 0; Index < Records.Num(); ++Index) {
    FString Key;
    HjsonWorkspace::FWorkspaceError Failure;
    if (!HjsonWorkspace::TryFormatReadableKeyedItemOrdinal(TEXT("Entry"), Index, Key, Failure)) {
      Error = Failure.GetDiagnostic(); return false;
    }
    auto Row = MakeShared<FJsonObject>();
    Row->SetStringField(TEXT("instance"), Key);
    Row->SetField(TEXT("settings"), ReadSettings(Records[Index], Asset));
    if (!Extra(Records[Index], *Row)) return false;
    Rows.Add(MakeShared<FJsonValueObject>(Row));
  }
  return true;
}

template<typename T, typename F>
bool ApplyRows(UAnimMontage& Asset, const FString& Field, const TSharedPtr<FJsonValue>& Value, TArray<T>& Records,
               F&& Extra, UE::UAI::Contract::FDiagnostic& Error, TArray<int32>* CapturedIndices,
               const FString& CanonicalAssetPath) {
  auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
  StateSemantic::FUeAgentCanonicalValuePtr Desired;
  TArray<HjsonWorkspace::FOrderedStructuredItemBinding> Bindings;
  HjsonWorkspace::FWorkspaceError Failure;
  if (!StateSemantic::CanonicalJson::TryFromJsonValue(Value, Resources, Desired, Error)) {
    UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error, TEXT("/") + Field); return false;
  }
  if (!HjsonWorkspace::TryResolveOrderedStructuredItems(*Desired, Records.Num(), Resources, Bindings, Failure)) {
    Error = Failure.GetDiagnostic();
    UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error, TEXT("/") + Field); return false;
  }
  TArray<T> After;
  if (CapturedIndices) CapturedIndices->Reset();
  for (const auto& Binding : Bindings) {
    T Record = Binding.CapturedIndex == INDEX_NONE ? T() : Records[Binding.CapturedIndex];
    TSharedPtr<FJsonValue> Json;
    if (!StateSemantic::CanonicalJson::TryToJsonValue(*Binding.Value, Json, Error)) {
      UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error,
          TEXT("/") + Field + TEXT("/") + FString::FromInt(After.Num())); return false;
    }
    const auto Row = Json->AsObject();
    if (!ApplySettings(Record, Asset, *Row, Error, CanonicalAssetPath) || !Extra(Record, *Row, Binding.CapturedIndex)) {
      UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error,
          TEXT("/") + Field + TEXT("/") + FString::FromInt(After.Num())); return false;
    }
    After.Add(MoveTemp(Record));
    if (CapturedIndices) CapturedIndices->Add(Binding.CapturedIndex);
  }
  Records = MoveTemp(After);
  return true;
}
}

TSharedPtr<FJsonObject> ReadMontageStructure(UAnimMontage& Asset, UE::UAI::Contract::FDiagnostic& Error) {
  auto Result = MakeShared<FJsonObject>();
  TArray<TSharedPtr<FJsonValue>> Slots, Sections;
  if (!ReadRows(Asset, Asset.SlotAnimTracks, Slots, [&](const FSlotAnimationTrack& Slot, FJsonObject& Row) {
    TArray<TSharedPtr<FJsonValue>> Segments;
    if (!ReadRows(Asset, Slot.AnimTrack.AnimSegments, Segments,
        [](const FAnimSegment&, FJsonObject&) { return true; }, Error)) return false;
    Row.SetArrayField(TEXT("segments"), Segments);
    return true;
  }, Error)) return nullptr;
  if (!ReadRows(Asset, Asset.CompositeSections, Sections, [&](const FCompositeSection& Section, FJsonObject& Row) {
    Row.SetNumberField(TEXT("time"), Section.GetTime());
    Row.SetStringField(TEXT("next_section"), Section.NextSectionName.ToString());
    UeAgentAuthoringState::FStateSnapshot Snapshot;
    TSharedPtr<FJsonObject> Properties;
    if (!ReadSectionProperties(Asset, Section, Snapshot, Properties, Error)) return false;
    Row.SetObjectField(TEXT("properties"), Properties);
    return true;
  }, Error)) return nullptr;
  Result->SetArrayField(TEXT("slots"), Slots);
  Result->SetArrayField(TEXT("sections"), Sections);
  Result->SetObjectField(TEXT("metadata"), UeAgentAuthoringObjectMetadata::Read(Asset));
  if (!ReadAnimationEvents(Asset, *Result, Error)) return nullptr;
  return Result;
}

bool ApplyMontageStructure(UAnimMontage& Asset, const TSharedPtr<const FJsonObject>& Document,
                           int32& Changes, UE::UAI::Contract::FDiagnostic& Error, const FString& CanonicalAssetPath) {
  const auto CapturedSections = Asset.CompositeSections;
  const auto CapturedNotifies = Asset.Notifies;
  if (const auto Slots = Document->TryGetField(TEXT("slots"))) {
    TMap<int32, TArray<int32>> SegmentSourcesBySlot;
    TArray<int32> SlotSources;
    if (!ApplyRows(Asset, TEXT("slots"), Slots, Asset.SlotAnimTracks, [&](FSlotAnimationTrack& Slot, const FJsonObject& Row, int32 SourceSlot) {
      const auto Segments = Row.TryGetField(TEXT("segments"));
      if (!Segments) return true;
      TArray<int32> SegmentSources;
      if (!ApplyRows(Asset, TEXT("segments"), Segments, Slot.AnimTrack.AnimSegments, [](FAnimSegment& Segment, const FJsonObject&, int32) {
        Segment.SetAnimReference(Segment.GetAnimReference());
        return true;
      }, Error, &SegmentSources, CanonicalAssetPath)) return false;
      if (SourceSlot != INDEX_NONE) SegmentSourcesBySlot.Add(SourceSlot, MoveTemp(SegmentSources));
      // SortAnimSegments is the exported editor entry; it validates lengths and
      // replaces these ordering keys with native contiguous start times.
      for (int32 Index = 0; Index < Slot.AnimTrack.AnimSegments.Num(); ++Index)
        Slot.AnimTrack.AnimSegments[Index].StartPos = float(Index);
      Slot.AnimTrack.SortAnimSegments();
      return true;
    }, Error, &SlotSources, CanonicalAssetPath)) return false;
    // Persona's OnAnimSegmentRemoved clears links to removed segments and shifts
    // surviving segment indices before UpdateLinkableElements. Ordered source
    // identities let the same rule handle multiple removals and reordering.
    const auto RemapSegment = [&](FAnimLinkableElement& Element) {
      const int32 OldSlot = Element.GetSlotIndex(), OldSegment = Element.GetSegmentIndex();
      const int32 NewSlot = SlotSources.IndexOfByKey(OldSlot);
      if (NewSlot == INDEX_NONE) return; // Native ConditionalRelink owns removed-slot fallback.
      if (NewSlot != OldSlot) {
        const auto Method = Element.GetLinkMethod();
        const float LinkTime = Element.GetTime(Method);
        Element.ChangeSlotIndex(NewSlot);
        // Changing the slot relinks by absolute time. Keep the authored relative
        // or proportional offset while the source segment changes index below.
        Element.FAnimLinkableElement::SetTime(LinkTime, Method);
        Element.SetSegmentIndex(OldSegment);
      }
      if (const auto* Sources = SegmentSourcesBySlot.Find(OldSlot)) {
        if (OldSegment == INDEX_NONE) return;
        const int32 NewIndex = Sources->IndexOfByKey(OldSegment);
        if (NewIndex == INDEX_NONE) Element.Clear();
        else Element.SetSegmentIndex(NewIndex);
      }
    };
    for (auto& Section : Asset.CompositeSections) RemapSegment(Section);
    for (auto& Notify : Asset.Notifies) {
      RemapSegment(Notify);
      if (Notify.NotifyStateClass) RemapSegment(Notify.EndLink);
    }
    Asset.InvalidateRecursiveAsset();
    Asset.UpdateLinkableElements();
    // Persona updates links before changing the data-model length. Length
    // notifications can relink notifies; doing that first bakes in old times.
    Asset.SetCompositeLength(Asset.CalculateSequenceLength());
    static_cast<UAnimCompositeBase&>(Asset).UpdateCommonTargetFrameRate();
    // Native slot deletion keeps the remaining slots and relinks invalid links.
    for (auto& Section : Asset.CompositeSections) Section.ConditionalRelink();
    for (auto& Notify : Asset.Notifies) {
      Notify.ConditionalRelink();
      if (Notify.NotifyStateClass) Notify.EndLink.ConditionalRelink();
    }
    ++Changes;
  }
  if (const auto Sections = Document->TryGetField(TEXT("sections"))) {
    const auto BeforeSections = Asset.CompositeSections;
    struct FSectionPropertyArguments {
      FName SectionName;
      FString BindingKey;
      TSharedPtr<FJsonValue> Source;
    };
    TArray<FSectionPropertyArguments> PropertyArguments;
    const bool bRecordArguments = UeAgentNativeNormalizationIntent::IsValueRecording();
    TArray<int32> SectionSources;
    if (!ApplyRows(Asset, TEXT("sections"), Sections, Asset.CompositeSections, [&](FCompositeSection& Section, const FJsonObject& Row, int32 SourceSection) {
      if (const auto Properties = Row.TryGetField(TEXT("properties"))) {
        TSharedPtr<FJsonObject> BoundRows;
        if (!ApplySectionProperties(Asset, Section, Properties, Error, CanonicalAssetPath,
            bRecordArguments ? &BoundRows : nullptr)) return false;
        if (BoundRows) PropertyArguments.Add({Section.SectionName, SectionBindingKey(SourceSection, SectionSources.Num()),
            MakeShared<FJsonValueObject>(BoundRows)});
      }
      if (Row.HasField(TEXT("next_section"))) {
        FString NextSection;
        if (!Row.TryGetStringField(TEXT("next_section"), NextSection)) {
          Error = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("animation_montage_next_section_name_expected"),
              TEXT("/next_section"), MakeShared<FJsonValueString>(TEXT("section name string")), Row.TryGetField(TEXT("next_section")),
              TEXT("original_intent"), TEXT("next_section must be a section name string.")); return false;
        }
        Section.NextSectionName = FName(*NextSection);
      }
      double Time = Section.GetTime();
      if ((Row.HasField(TEXT("time")) && !Row.TryGetNumberField(TEXT("time"), Time)) || !FMath::IsFinite(Time)) {
        Error = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("animation_montage_section_time_invalid"),
            TEXT("/time"), MakeShared<FJsonValueString>(TEXT("finite number")), Row.HasField(TEXT("time"))
                ? Row.TryGetField(TEXT("time")) : MakeShared<FJsonValueString>(LexToString(Time)),
            TEXT("original_intent"), TEXT("A montage section time must be a finite number."));
        if (!Row.HasField(TEXT("time"))) Error.SourceKind = TEXT("native_result");
        return false;
      }
      float NativeTime = 0;
      if (!UeAgentPropertyValueCodec::TryConvertFloat32(Time, NativeTime, Error)) {
        if (Row.HasField(TEXT("time"))) UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error, TEXT("/time"));
        else Error.SourceKind = TEXT("native_result");
        return false;
      }
      if (!CapturedSections.IsValidIndex(SourceSection) ||
          (Row.HasField(TEXT("time")) && NativeTime != CapturedSections[SourceSection].GetTime()))
        Section.Link(&Asset, NativeTime, Section.GetSlotIndex());
      return true;
    }, Error, &SectionSources, CanonicalAssetPath)) return false;
    TSet<FName> RemovedNames;
    for (int32 Index = 0; Index < BeforeSections.Num(); ++Index)
      if (!SectionSources.Contains(Index)) RemovedNames.Add(BeforeSections[Index].SectionName);
    for (int32 Index = 0; Index < Asset.CompositeSections.Num(); ++Index) {
      auto& Section = Asset.CompositeSections[Index];
      if (!RemovedNames.Contains(Section.NextSectionName)) continue;
      if (BeforeSections.IsValidIndex(SectionSources[Index]) &&
          BeforeSections[SectionSources[Index]].NextSectionName == Section.NextSectionName) {
        // Persona clears incoming links when removing a composite section.
        Section.NextSectionName = NAME_None;
      } else {
        Error = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("animation_montage_next_section_removed"),
            TEXT("/sections/") + FString::FromInt(Index) + TEXT("/next_section"),
            MakeShared<FJsonValueString>(TEXT("section name retained in this montage")),
            MakeShared<FJsonValueString>(Section.NextSectionName.ToString()), TEXT("original_intent"),
            TEXT("The section links to a section removed by this edit."));
        Error.SubjectId = Section.SectionName.ToString();
        return false;
      }
    }
    Asset.CompositeSections.Sort([](const FCompositeSection& A, const FCompositeSection& B) { return A.GetTime() < B.GetTime(); });
    UAnimMontageFactory::EnsureStartingSection(&Asset);
    for (const auto& Arguments : PropertyArguments) {
      const int32 NativeIndex = Asset.GetSectionIndex(Arguments.SectionName);
      const FString ComparisonPath = TEXT("/sections/") + Arguments.BindingKey + TEXT("/properties");
      const FString NativePath = TEXT("/sections/") + FString::FromInt(NativeIndex) + TEXT("/properties");
      if (!UeAgentNativeNormalizationIntent::RecordValue(ComparisonPath, NativePath, Arguments.Source, Arguments.Source, Error))
        return false;
    }
    ++Changes;
  }
  if (!ApplyAnimationEvents(Asset, *Document, Changes, Error, false, &CapturedNotifies, CanonicalAssetPath)) return false;
  if (const auto Metadata = Document->TryGetField(TEXT("metadata"))) {
    if (Metadata->Type != EJson::Object) {
      Error = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("animation_metadata_object_expected"), TEXT("/metadata"),
          MakeShared<FJsonValueString>(TEXT("object")), Metadata, TEXT("original_intent"), TEXT("Animation metadata must be an object."));
      return false;
    }
    if (!UeAgentAuthoringObjectMetadata::ApplyDesired(Asset, *Metadata->AsObject(), Error)) {
      UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error, TEXT("/metadata")); return false;
    }
  }
  Asset.RefreshCacheData();
  return true;
}

bool ProjectMontageOriginalIntent(UeAgentAuthoringState::FStateEntry& NativeEntry, TSharedPtr<FJsonValue>& Before,
    TSharedPtr<FJsonValue>& Desired, TSharedPtr<FJsonValue>& Actual,
    TMap<FString, FString>& SourcePaths, UE::UAI::Contract::FDiagnostic& Error) {
  if (!Before || !Desired || !Actual || Before->Type != EJson::Object ||
      Desired->Type != EJson::Object || Actual->Type != EJson::Object) return true;
  const TArray<TSharedPtr<FJsonValue>> *BeforeRows = nullptr, *DesiredRows = nullptr, *ActualRows = nullptr;
  if (!Before->AsObject()->TryGetArrayField(TEXT("sections"), BeforeRows) ||
      !Desired->AsObject()->TryGetArrayField(TEXT("sections"), DesiredRows) ||
      !Actual->AsObject()->TryGetArrayField(TEXT("sections"), ActualRows)) return true;
  auto Resources = HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
  StateSemantic::FUeAgentCanonicalValuePtr Rows;
  TArray<HjsonWorkspace::FOrderedStructuredItemBinding> Bindings;
  HjsonWorkspace::FWorkspaceError Failure;
  if (!StateSemantic::CanonicalJson::TryFromJsonValue(
      MakeShared<FJsonValueArray>(*DesiredRows), Resources, Rows, Error) ||
      !HjsonWorkspace::TryResolveOrderedStructuredItems(*Rows, BeforeRows->Num(), Resources, Bindings, Failure)) {
    if (Error.IsEmpty()) Error = Failure.GetDiagnostic();
    return false;
  }
  auto SectionName = [](const TSharedPtr<FJsonValue>& Value) {
    const TSharedPtr<FJsonObject> *Settings = nullptr, *Fields = nullptr;
    FString Name;
    if (Value && Value->Type == EJson::Object &&
        Value->AsObject()->TryGetObjectField(TEXT("settings"), Settings) &&
        (*Settings)->TryGetObjectField(TEXT("fields"), Fields))
      (*Fields)->TryGetStringField(TEXT("SectionName"), Name);
    return FName(*Name);
  };
  auto ProjectRow = [&](const TSharedPtr<FJsonValue>& Value) -> TSharedPtr<FJsonValue> {
    const auto Row = UeAgentAuthoringState::CloneCanonicalValue(Value)->AsObject();
    Row->RemoveField(TEXT("instance"));
    double Time = 0;
    Row->TryGetNumberField(TEXT("time"), Time);
    float NativeTime = 0;
    if (!UeAgentPropertyValueCodec::TryConvertFloat32(Time, NativeTime, Error)) return nullptr;
    Row->SetNumberField(TEXT("time"), NativeTime);
    const TSharedPtr<FJsonObject> *Settings = nullptr, *ExistingFields = nullptr;
    if (!Row->TryGetObjectField(TEXT("settings"), Settings)) {
      Row->SetObjectField(TEXT("settings"), MakeShared<FJsonObject>());
      Row->TryGetObjectField(TEXT("settings"), Settings);
    }
    if (!(*Settings)->TryGetObjectField(TEXT("fields"), ExistingFields)) {
      (*Settings)->SetObjectField(TEXT("fields"), MakeShared<FJsonObject>());
      (*Settings)->TryGetObjectField(TEXT("fields"), ExistingFields);
    }
    auto Fields = *ExistingFields;
    FName Name = NAME_None;
    if (const auto NameValue = Fields->TryGetField(TEXT("SectionName"))) {
      // State captures retain codec literals; the public workspace may use
      // compact strings. The field's native codec owns both representations.
      static auto* NameProperty = FindFProperty<FNameProperty>(FCompositeSection::StaticStruct(), TEXT("SectionName"));
      if (!UeAgentPropertyValueCodec::Decode(NameProperty, NameValue, &Name, nullptr, {}, Error)) return nullptr;
    }
    Fields->SetStringField(TEXT("SectionName"), Name.ToString());
    return MakeShared<FJsonValueObject>(Row);
  };
  auto BeforeBySource = MakeShared<FJsonObject>();
  auto DesiredBySource = MakeShared<FJsonObject>();
  auto ActualBySource = MakeShared<FJsonObject>();
  TMap<FName, FString> SourceByName;
  for (int32 Index = 0; Index < BeforeRows->Num(); ++Index) {
    const FString Key = FString::FromInt(Index);
    auto Row = ProjectRow((*BeforeRows)[Index]);
    if (!Row) return false;
    BeforeBySource->SetField(Key, Row);
	SourcePaths.Add(TEXT("/sections/") + Key, TEXT("/sections"));
    SourceByName.Add(SectionName(Row), Key);
  }
  TArray<TSharedPtr<FJsonValue>> ProjectedActual;
  for (const auto& Native : *ActualRows) {
    auto Row = ProjectRow(Native);
    if (!Row) return false;
    ProjectedActual.Add(Row);
  }
  float FirstTime = MAX_flt;
  FString FirstKey;
  for (int32 Index = 0; Index < Bindings.Num(); ++Index) {
    const auto& Binding = Bindings[Index];
    TSharedPtr<FJsonValue> Json;
    if (!StateSemantic::CanonicalJson::TryToJsonValue(*Binding.Value, Json, Error)) return false;
    auto Row = ProjectRow(Json);
    if (!Row) return false;
    const FString Key = SectionBindingKey(Binding.CapturedIndex, Index);
    DesiredBySource->SetField(Key, Row);
    SourcePaths.Add(TEXT("/sections/") + Key, TEXT("/sections/") + FString::FromInt(Index));
    SourceByName.Add(SectionName(Row), Key);
    double Time = Row->AsObject()->GetNumberField(TEXT("time"));
    const bool bTimeChanged = Binding.CapturedIndex == INDEX_NONE ||
        !StateSemantic::CanonicalJson::AreJsonValuesEquivalent(
            (*BeforeRows)[Binding.CapturedIndex]->AsObject()->TryGetField(TEXT("time")), Json->AsObject()->TryGetField(TEXT("time")));
    if (Binding.CapturedIndex != INDEX_NONE && bTimeChanged) {
      // Preserve the original write mask even when native normalization brings
      // the requested time back to its previous value (first section -> zero).
      BeforeBySource->GetObjectField(Key)->RemoveField(TEXT("time"));
    }
    if (!bTimeChanged) {
      // Untouched link times may legitimately move with a changed segment.
      for (const auto& Native : ProjectedActual)
        if (SectionName(Native) == SectionName(Row)) { Time = Native->AsObject()->GetNumberField(TEXT("time")); break; }
    }
    if (Time < FirstTime) { FirstTime = float(Time); FirstKey = Key; }
  }
  if (Bindings.IsEmpty()) {
    // Use the native default owner, rather than copying a readback value as
    // the expectation for an explicitly emptied section collection.
    auto* Defaults = NewObject<UAnimMontage>(GetTransientPackage(), NAME_None, RF_Transient);
    UAnimMontageFactory::EnsureStartingSection(Defaults);
    auto Document = ReadMontageStructure(*Defaults, Error);
    if (!Document) return false;
    for (const auto& Row : Document->GetArrayField(TEXT("sections"))) {
      const auto Projected = ProjectRow(Row);
      if (!Projected) return false;
      const FString Key = TEXT("native_default:") + SectionName(Projected).ToString();
      DesiredBySource->SetField(Key, Projected);
      SourceByName.Add(SectionName(Projected), Key);
    }
  } else if (FirstTime > 0) {
    // EnsureStartingSection's documented native normalization.
    // Equal-time rows have no stable native secondary ordering.
    if (!ProjectedActual.IsEmpty()) {
      const auto* NativeFirstKey = SourceByName.Find(SectionName(ProjectedActual[0]));
      if (NativeFirstKey && DesiredBySource->HasField(*NativeFirstKey) &&
          DesiredBySource->GetObjectField(*NativeFirstKey)->GetNumberField(TEXT("time")) == FirstTime)
        FirstKey = *NativeFirstKey;
    }
    DesiredBySource->GetObjectField(FirstKey)->SetNumberField(TEXT("time"), 0.0);
  }
  TMap<FString, int32> SourceToNativeIndex;
  for (int32 Index = 0; Index < ProjectedActual.Num(); ++Index) {
    const auto& Native = ProjectedActual[Index];
    const auto* Source = SourceByName.Find(SectionName(Native));
    const FString Key = Source ? *Source : TEXT("unexpected:") + FString::FromInt(Index);
    ActualBySource->SetField(Key, Native);
    SourceToNativeIndex.Add(Key, Index);
    if (!Source) BeforeBySource->SetObjectField(Key, MakeShared<FJsonObject>());
  }
  if (!UeAgentAuthoringSemanticCompare::BindComparisonCollectionKeys(
      NativeEntry, TEXT("sections"), SourceToNativeIndex, Error)) return false;
  Before = UeAgentAuthoringState::CloneCanonicalValue(Before);
  Desired = UeAgentAuthoringState::CloneCanonicalValue(Desired);
  Actual = UeAgentAuthoringState::CloneCanonicalValue(Actual);
  Before->AsObject()->SetObjectField(TEXT("sections"), BeforeBySource);
  Desired->AsObject()->SetObjectField(TEXT("sections"), DesiredBySource);
  Actual->AsObject()->SetObjectField(TEXT("sections"), ActualBySource);
  return true;
}
}
