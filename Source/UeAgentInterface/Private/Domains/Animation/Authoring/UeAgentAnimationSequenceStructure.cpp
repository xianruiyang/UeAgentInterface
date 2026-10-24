// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Animation/Authoring/UeAgentAnimationSequenceStructure.h"
#include "Domains/Animation/Authoring/UeAgentAnimationEvents.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringEncodedTypes.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringJsonFields.h"
#include "Utilities/AuthoringCore/UeAgentNativeNormalizationIntent.h"
#include "JsonUtils/JsonPointer.h"

#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimData/CurveIdentifier.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimTypes.h"
#include "Animation/Skeleton.h"
#include "Curves/RealCurve.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FrameRate.h"
#include "Misc/ScopeExit.h"
#include "UObject/Class.h"
#include "UObject/NoExportTypes.h"
#include "Utilities/UeAgentCurveJson.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Utilities/AuthoringObjectApply/UeAgentAuthoringObjectApply.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringObjectMetadata.h"

namespace UeAgentAnimationAuthoring {
namespace {
using FCodecOptions = UeAgentPropertyValueCodec::FOptions;
const TCHAR* TransformChannels[] = {TEXT("position"), TEXT("rotation"), TEXT("scale")};
const TCHAR* VectorAxes[] = {TEXT("x"), TEXT("y"), TEXT("z")};

UE::UAI::Contract::FDiagnostic NativeFailure(const FString& Code,
    const FString& Reason, const FString& Subject = FString()) {
  auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("animation_apply"));
  Diagnostic.SourceKind = TEXT("native_result");
  Diagnostic.SubjectId = Subject;
  return Diagnostic;
}

UE::UAI::Contract::FDiagnostic ControllerFailure(const FString& Code,
    const FString& Reason, const FName Name, const FString& Operation) {
  auto Diagnostic = NativeFailure(Code, Reason, Name.ToString());
  Diagnostic.PropertyPath = Operation;
  const auto Actual = MakeShared<FJsonObject>();
  Actual->SetStringField(TEXT("record"), Name.ToString());
  Actual->SetBoolField(TEXT("controller_accepted"), false);
  Diagnostic.ActualValue = MakeShared<FJsonValueObject>(Actual);
  const auto Expected = MakeShared<FJsonObject>();
  Expected->SetBoolField(TEXT("controller_accepted"), true);
  Diagnostic.ExpectedValue = MakeShared<FJsonValueObject>(Expected);
  Diagnostic.MutationMayHaveStarted = true;
  return Diagnostic;
}

TSharedPtr<FJsonValue> FrameRateValue(const FFrameRate& Rate) {
  const auto Value = MakeShared<FJsonObject>();
  Value->SetNumberField(TEXT("numerator"), Rate.Numerator);
  Value->SetNumberField(TEXT("denominator"), Rate.Denominator);
  return MakeShared<FJsonValueObject>(Value);
}

FAnimationCurveIdentifier TransformChannel(FName Name, int32 Channel, int32 Axis) {
  FAnimationCurveIdentifier Id(Name, ERawCurveTrackTypes::RCT_Transform);
  Id.Channel = static_cast<ETransformCurveChannel>(Channel);
  Id.Axis = static_cast<EVectorCurveChannel>(Axis);
  return Id;
}

TSharedPtr<FJsonObject> ReadCurve(const FRichCurve& Curve, bool bAttributes = true) {
  auto Result = UeAgentCurveJson::BuildFloatCurveJson(Curve, TEXT(""), TEXT(""), true);
  // Animation's controller exposes keys and extrapolation, not curve defaults.
  Result->GetObjectField(TEXT("channels"))->GetObjectField(TEXT("value"))->RemoveField(TEXT("default_value"));
  // The native Sequencer controller exposes extrapolation edits only for scalar curves.
  if (!bAttributes) {
    Result->RemoveField(TEXT("pre_infinity_extrap"));
    Result->RemoveField(TEXT("post_infinity_extrap"));
  }
  return Result;
}

bool DecodeCurve(const TSharedPtr<FJsonValue>& Value, FRichCurve& Curve,
                 const FString& Path, bool bAttributes, UE::UAI::Contract::FDiagnostic& OutError) {
  const TSharedPtr<FJsonObject>* Object = nullptr;
  if (!Value || !Value->TryGetObject(Object) || !Object || !Object->IsValid()) {
    OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("animation_curve_expected_object"), Path,
        MakeShared<FJsonValueString>(TEXT("curve object")), Value, TEXT("curve_decode"), TEXT("An animation curve must be an object containing channels.value."));
    return false;
  }
  auto RejectUnsupported = [&](const TSharedPtr<FJsonObject>& Container, const FString& Field,
                               const FString& FieldPath, const FString& Code, const FString& Reason) {
    if (!Container || !Container->HasField(Field)) return false;
    OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, FieldPath,
        MakeShared<FJsonValueString>(TEXT("field omitted")), Container->TryGetField(Field), TEXT("curve_decode"), Reason);
    return true;
  };
  const TSharedPtr<FJsonObject>* Channels = nullptr;
  const TSharedPtr<FJsonObject>* Channel = nullptr;
  if ((*Object)->TryGetObjectField(TEXT("channels"), Channels) && Channels && *Channels)
    (*Channels)->TryGetObjectField(TEXT("value"), Channel);
  const auto ChannelObject = Channel ? *Channel : nullptr;
  if (RejectUnsupported(ChannelObject, TEXT("default_value"), Path + TEXT("/channels/value/default_value"),
      TEXT("animation_curve_default_not_supported_by_controller"),
      TEXT("Animation curves do not expose a writable default_value. Omit it and author keys instead."))) return false;
  if (!bAttributes) {
    for (const FString Field : {FString(TEXT("pre_infinity_extrap")), FString(TEXT("post_infinity_extrap"))}) {
      const FString Reason = TEXT("The animation controller cannot edit extrapolation on transform-curve channels. Omit this field.");
      if (RejectUnsupported(*Object, Field, Path + TEXT("/") + Field,
            TEXT("animation_transform_curve_extrapolation_not_supported"), Reason)
          || RejectUnsupported(ChannelObject, Field, Path + TEXT("/channels/value/") + Field,
            TEXT("animation_transform_curve_extrapolation_not_supported"), Reason)) return false;
    }
  }
  TArray<UE::UAI::Contract::FDiagnostic> Issues;
  if (!UeAgentCurveJson::ApplyChannelJsonToRichCurve(*Object, TEXT("value"), Curve, Path, Issues)) {
    OutError = UeAgentAuthoringDiagnostics::MakeIssuesFailure(
        TEXT("animation_curve_invalid"), Issues, TEXT("curve_decode"));
    return false;
  }
  return true;
}

bool WriteCurve(IAnimationDataController& Controller, const FAnimationCurveIdentifier& Id,
                const FRichCurve& Curve, const FString& IntentPath, UE::UAI::Contract::FDiagnostic& OutError) {
  const bool bRecord = UeAgentNativeNormalizationIntent::IsValueRecording();
  const bool bAttributes = Id.CurveType == ERawCurveTrackTypes::RCT_Float;
  TSharedPtr<FJsonValue> Source;
  if (bRecord) Source = MakeShared<FJsonValueObject>(ReadCurve(Curve, bAttributes));
  FCurveAttributes Attributes;
  Attributes.SetPreExtrapolation(Curve.PreInfinityExtrap);
  Attributes.SetPostExtrapolation(Curve.PostInfinityExtrap);
  if (!Controller.SetCurveKeys(Id, Curve.GetConstRefOfKeys(), false)) {
    OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_curve_controller_keys_failed"), TEXT("The native controller could not write the curve keys."), TEXT("animation_apply"), FString::Printf(TEXT("%s:type=%d:channel=%d:axis=%d:transforms=%d:readable=%d:model=%s"),
        *Id.CurveName.ToString(), int32(Id.CurveType), int32(Id.Channel), int32(Id.Axis),
        Controller.GetModel()->GetTransformCurves().Num(), Controller.GetModel()->FindRichCurve(Id) != nullptr,
        *Controller.GetModelInterface().GetObject()->GetClass()->GetPathName()));
    OutError.SemanticPointer = IntentPath;
    OutError.Source.JsonPath = IntentPath;
    OutError.SourceKind = TEXT("native_result");
    OutError.SubjectId = Id.CurveName.ToString();
    OutError.PropertyPath = TEXT("curve_keys");
    const auto Actual = MakeShared<FJsonObject>();
    Actual->SetBoolField(TEXT("controller_accepted"), false);
    Actual->SetNumberField(TEXT("requested_key_count"), Curve.GetConstRefOfKeys().Num());
    OutError.ActualValue = MakeShared<FJsonValueObject>(Actual);
    const auto Expected = MakeShared<FJsonObject>();
    Expected->SetBoolField(TEXT("controller_accepted"), true);
    OutError.ExpectedValue = MakeShared<FJsonValueObject>(Expected);
    OutError.MutationMayHaveStarted = true;
    return false;
  }
  const FRichCurve* Written = Controller.GetModel()->FindRichCurve(Id);
  if (!Written || Written->PreInfinityExtrap != Curve.PreInfinityExtrap ||
      Written->PostInfinityExtrap != Curve.PostInfinityExtrap) {
    if (!Controller.SetCurveAttributes(Id, Attributes, false)) {
      OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_curve_controller_attributes_failed"), TEXT("The native controller could not write the curve attributes."), TEXT("animation_apply"), FString::Printf(TEXT("%s:channel=%d:axis=%d:readable=%d"),
          *Id.CurveName.ToString(), int32(Id.Channel), int32(Id.Axis), Written != nullptr));
      OutError.SemanticPointer = IntentPath;
      OutError.Source.JsonPath = IntentPath;
      OutError.SourceKind = TEXT("native_result");
      OutError.SubjectId = Id.CurveName.ToString();
      OutError.PropertyPath = TEXT("curve_attributes");
      OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
      OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
      OutError.MutationMayHaveStarted = true;
      return false;
    }
  }
  if (bRecord) {
    const FRichCurve* Normalized = Controller.GetModel()->FindRichCurve(Id);
    if (!Normalized) {
      auto Warning = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_curve_native_readback_missing"), TEXT("The native curve was not available for readback."), TEXT("native_normalization_observation"));
      Warning.SemanticPointer = IntentPath;
      Warning.Source.JsonPath = IntentPath;
      Warning.SourceKind = TEXT("native_result");
      Warning.SubjectId = Id.CurveName.ToString();
      Warning.ActualValue = MakeShared<FJsonValueNull>();
      Warning.ExpectedValue = MakeShared<FJsonValueString>(TEXT("readable native curve"));
      UeAgentAuthoringDiagnostics::ReportConsistencyWarning(Warning);
    }
    else if (!UeAgentNativeNormalizationIntent::RecordValue(IntentPath, Source,
          MakeShared<FJsonValueObject>(ReadCurve(*Normalized, bAttributes)), OutError)) {
      OutError.SemanticPointer = IntentPath;
      OutError.Source.JsonPath = IntentPath;
      UeAgentAuthoringDiagnostics::ReportConsistencyWarning(FString(), OutError, TEXT("native_normalization_observation"));
      OutError.Reset();
    }
  }
  return true;
}

TSharedPtr<FJsonValue> EncodeTrack(const FRawAnimSequenceTrack& Track, UAnimSequence& Sequence) {
  return UeAgentPropertyValueCodec::EncodeStructValue(
      FRawAnimSequenceTrack::StaticStruct(), &Track, &Sequence, FCodecOptions());
}

FRawAnimSequenceTrack ReadTrack(const IAnimationDataModel& Model, FName Name) {
  TArray<FTransform> Transforms;
  Model.GetBoneTrackTransforms(Name, Transforms);
  FRawAnimSequenceTrack Track;
  for (const FTransform& Transform : Transforms) {
    Track.PosKeys.Add(FVector3f(Transform.GetTranslation()));
    Track.RotKeys.Add(FQuat4f(Transform.GetRotation()));
    Track.ScaleKeys.Add(FVector3f(Transform.GetScale3D()));
  }
  return Track;
}

bool DecodeTrack(const TSharedPtr<FJsonValue>& Value, FRawAnimSequenceTrack& Track,
                 UAnimSequence& Sequence, UE::UAI::Contract::FDiagnostic& OutError,
                 const FString& CanonicalAssetPath) {
  const TSharedPtr<FJsonObject>* Object = nullptr;
  const TSharedPtr<FJsonObject>* Fields = nullptr;
  if (!Value || !Value->TryGetObject(Object) || !Object ||
      !(*Object)->TryGetObjectField(TEXT("fields"), Fields) || !Fields) {
    const bool bTrackObject = Object && Object->IsValid();
    OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("animation_bone_track_fields_expected"),
        bTrackObject ? TEXT("/fields") : TEXT("/"), MakeShared<FJsonValueString>(TEXT("object")),
        bTrackObject ? (*Object)->TryGetField(TEXT("fields")) : Value, TEXT("animation_apply"),
        bTrackObject ? TEXT("The encoded bone track requires a fields object.")
            : TEXT("Each bone track must be an encoded track object."));
    return false;
  }
  TArray<TSharedPtr<FJsonValue>> Descriptors;
  for (const auto& Field : (*Fields)->Values) {
    const auto Descriptor = MakeShared<FJsonObject>();
    Descriptor->SetStringField(TEXT("property_path"), Field.Key);
    Descriptor->SetField(TEXT("value"), Field.Value);
    Descriptors.Add(MakeShared<FJsonValueObject>(Descriptor));
  }
  UeAgentAuthoringObjectApply::FOptions Options;
  Options.DiagnosticTargetPath = CanonicalAssetPath;
  Options.bCompactValues = true;
  Options.bEmitEditorNotifications = false;
  Options.bMarkPackageDirty = false;
  Options.AuthorizeProperty = [](const FProperty* Property) {
    return Property && Property->GetOwnerStruct() == FRawAnimSequenceTrack::StaticStruct();
  };
  UeAgentAuthoringObjectApply::FPreparedApply Prepared;
  UeAgentAuthoringObjectApply::FFailure Failure;
  UeAgentAuthoringObjectApply::FApplyResult Applied;
  if (!UeAgentAuthoringObjectApply::PrepareStruct(FRawAnimSequenceTrack::StaticStruct(),
          &Track, &Sequence, Descriptors, Options, Prepared, Failure)) {
    OutError = Failure.Diagnostic;
    UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, TEXT("/fields"), true);
    return false;
  }
  if (!UeAgentAuthoringObjectApply::ApplyPrepared(Prepared, Applied)) {
    OutError = Applied.Diagnostic;
    UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, TEXT("/fields"), true);
    return false;
  }
  return true;
}

// A constant component is stored once by UE. The controller consumes equally
// sized components; expand only edited tracks, preserving all other model data.
template<typename T>
bool ExpandConstant(TArray<T>& Keys, int32 Count) {
  if (Keys.Num() == 1 && Count > 1) {
    const T Value = Keys[0];
    Keys.Init(Value, Count);
  }
  return Keys.Num() == Count;
}
}

TSharedPtr<FJsonObject> BuildSequenceCurveAuthoringExample(bool bTransformChannel, float Value) {
  FRichCurve Curve;
  Curve.AddKey(0.f, Value);
  return ReadCurve(Curve, !bTransformChannel);
}

TSharedPtr<FJsonObject> ReadSequenceStructure(UAnimSequence& Sequence, UE::UAI::Contract::FDiagnostic& OutError) {
  const IAnimationDataModel* Model = Sequence.GetDataModel();
  if (!Model) {
    OutError = NativeFailure(TEXT("animation_sequence_data_model_missing"), TEXT("The animation sequence has no data model to read."));
    OutError.InternalContext = Sequence.GetPathName();
    OutError.ActualValue = MakeShared<FJsonValueNull>();
    OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("IAnimationDataModel"));
    return nullptr;
  }
  const auto Document = MakeShared<FJsonObject>();
  Document->SetStringField(TEXT("skeleton"), Sequence.GetSkeleton() ? Sequence.GetSkeleton()->GetPathName() : FString());
  const FFrameRate FrameRate = Model->GetFrameRate();
  Document->SetField(TEXT("frame_rate"), UeAgentPropertyValueCodec::EncodeStructValue(
      TBaseStructure<FFrameRate>::Get(), &FrameRate, &Sequence, FCodecOptions()));
  Document->SetNumberField(TEXT("number_of_frames"), Model->GetNumberOfFrames());
  const auto Tracks = MakeShared<FJsonObject>();
  TArray<FName> Names;
  Model->GetBoneTrackNames(Names);
  for (FName Name : Names) {
    Tracks->SetField(Name.ToString(), EncodeTrack(ReadTrack(*Model, Name), Sequence));
  }
  UeAgentAuthoringEncodedTypes::FScope::RecordNameKeyedObject(Tracks);
  Document->SetObjectField(TEXT("bone_tracks"), Tracks);
  const auto FloatCurves = MakeShared<FJsonObject>();
  for (const FFloatCurve& Curve : Model->GetFloatCurves())
    FloatCurves->SetObjectField(Curve.GetName().ToString(), ReadCurve(Curve.FloatCurve));
  UeAgentAuthoringEncodedTypes::FScope::RecordNameKeyedObject(FloatCurves);
  Document->SetObjectField(TEXT("float_curves"), FloatCurves);
  const auto TransformCurves = MakeShared<FJsonObject>();
  for (const FTransformCurve& Curve : Model->GetTransformCurves()) {
    const auto Value = MakeShared<FJsonObject>();
    for (int32 Channel = 0; Channel < 3; ++Channel) {
      const auto Axes = MakeShared<FJsonObject>();
      for (int32 Axis = 0; Axis < 3; ++Axis)
        Axes->SetObjectField(VectorAxes[Axis], ReadCurve(Curve.GetVectorCurveByIndex(Channel)->FloatCurves[Axis], false));
      Value->SetObjectField(TransformChannels[Channel], Axes);
    }
    TransformCurves->SetObjectField(Curve.GetName().ToString(), Value);
  }
  UeAgentAuthoringEncodedTypes::FScope::RecordNameKeyedObject(TransformCurves);
  Document->SetObjectField(TEXT("transform_curves"), TransformCurves);
  Document->SetObjectField(TEXT("metadata"), UeAgentAuthoringObjectMetadata::Read(Sequence));
  if (!ReadAnimationEvents(Sequence, *Document, OutError)) return nullptr;
  return Document;
}

bool ApplySequenceStructure(UAnimSequence& Sequence,
                            const TSharedPtr<const FJsonObject>& Document,
                            int32& OutChanges, UE::UAI::Contract::FDiagnostic& OutError,
                            const FString& CanonicalAssetPath) {
  OutChanges = 0;
  IAnimationDataModel* Model = Sequence.GetDataModel();
  if (!Model) {
    OutError = NativeFailure(TEXT("animation_sequence_data_model_missing"), TEXT("The animation sequence has no data model to edit."));
    OutError.InternalContext = Sequence.GetPathName();
    OutError.ActualValue = MakeShared<FJsonValueNull>();
    OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("IAnimationDataModel"));
    return false;
  }
  const FJsonValueString CurrentSkeleton(Sequence.GetSkeleton() ? Sequence.GetSkeleton()->GetPathName() : FString());
  if (Document->HasField(TEXT("skeleton")) &&
      !FJsonValue::CompareEqual(*Document->TryGetField(TEXT("skeleton")), CurrentSkeleton)) {
    OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("animation_sequence_skeleton_context_changed"),
        TEXT("/skeleton"), MakeShared<FJsonValueString>(CurrentSkeleton.AsString()), Document->TryGetField(TEXT("skeleton")),
        TEXT("animation_apply"), TEXT("The supplied skeleton reference differs from the sequence's bound skeleton."));
    return false;
  }
  FFrameRate FrameRate = Model->GetFrameRate();
  FCodecOptions Options;
  Options.bAllowPartialStructFields = true;
  if (Document->HasField(TEXT("frame_rate")) &&
      !UeAgentPropertyValueCodec::DecodeStructValue(TBaseStructure<FFrameRate>::Get(),
          Document->TryGetField(TEXT("frame_rate")), &FrameRate, &Sequence, Options, OutError)) {
    UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, TEXT("/frame_rate"));
    return false;
  }
  int32 Frames = Model->GetNumberOfFrames();
  if (!UeAgentAuthoringJsonFields::ReadInt32Field(*Document, TEXT("number_of_frames"), Frames, OutError)) return false;
  if (FrameRate.Numerator <= 0 || FrameRate.Denominator <= 0 || Frames < 1) {
    const bool bRateInvalid = FrameRate.Numerator <= 0 || FrameRate.Denominator <= 0;
    const FString Field = bRateInvalid ? TEXT("frame_rate") : TEXT("number_of_frames");
    FString Pointer = TEXT("/") + Field;
    TSharedPtr<FJsonValue> Actual = bRateInvalid ? FrameRateValue(FrameRate) : MakeShared<FJsonValueNumber>(Frames);
    FString Expected = bRateInvalid ? TEXT("positive numerator and denominator") : TEXT("integer >= 1");
    if (bRateInvalid) {
      const auto InputRate = Document->TryGetField(TEXT("frame_rate"));
      for (const TCHAR* Member : {TEXT("Numerator"), TEXT("Denominator")}) {
        const int32 Value = FCString::Strcmp(Member, TEXT("Numerator")) == 0 ? FrameRate.Numerator : FrameRate.Denominator;
        if (Value > 0) continue;
        for (const FString& Relative : {TEXT("/fields/") + FString(Member), TEXT("/") + FString(Member)}) {
          TSharedPtr<FJsonValue> InputValue;
          if (InputRate && UE::Json::FJsonPointer(Relative).TryGet(InputRate, InputValue)) {
            Pointer += Relative;
            Actual = MakeShared<FJsonValueNumber>(Value);
            Expected = TEXT("integer > 0");
            break;
          }
        }
        if (Pointer != TEXT("/frame_rate")) break;
      }
    }
    OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("animation_sequence_time_domain_invalid"),
        Pointer, MakeShared<FJsonValueString>(Expected), Actual, TEXT("animation_apply"),
        bRateInvalid ? TEXT("The animation frame rate requires a positive numerator and denominator.")
            : TEXT("The animation sequence requires at least one frame."));
    if (!Document->HasField(Field)) OutError.SourceKind = TEXT("native_result");
    return false;
  }

  const TSharedPtr<FJsonObject>* RequestedTracks = nullptr;
  TMap<FName, FRawAnimSequenceTrack> Desired;
  TSet<FName> Changed;
  struct FTrackSource {
    FString Path;
    FIntVector KeyCounts;
    TSharedPtr<FJsonValue> Value;
    TSharedPtr<FJsonValue> Definition;
  };
  TMap<FName, FTrackSource> TrackSources;
  const bool bRecordTrackIntent = UeAgentNativeNormalizationIntent::IsValueRecording();
  if (Document->HasField(TEXT("bone_tracks"))) {
    if (!Document->TryGetObjectField(TEXT("bone_tracks"), RequestedTracks) || !RequestedTracks) {
      OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("animation_sequence_bone_tracks_expected_object"),
          TEXT("/bone_tracks"), MakeShared<FJsonValueString>(TEXT("object keyed by bone name")), Document->TryGetField(TEXT("bone_tracks")),
          TEXT("animation_apply"), TEXT("bone_tracks must be an object keyed by bone name."));
      return false;
    }
    for (const auto& Pair : (*RequestedTracks)->Values) {
      const FName Name(*Pair.Key);
      if (Name.IsNone() || Desired.Contains(Name)) {
        OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("animation_sequence_bone_name_invalid"),
            TEXT("/bone_tracks/") + UE::Json::FJsonPointer::EscapePart(Pair.Key),
            MakeShared<FJsonValueString>(Name.IsNone() ? TEXT("non-empty bone name other than None") : TEXT("unique bone name ignoring case")),
            MakeShared<FJsonValueString>(Pair.Key), TEXT("animation_apply"),
            Name.IsNone() ? TEXT("The bone track name must not resolve to None.")
                : TEXT("The bone track name duplicates another name under Unreal's case-insensitive name comparison."));
        return false;
      }
      const bool bExisting = Model->IsValidBoneTrackName(Name);
      const FRawAnimSequenceTrack Existing = bExisting ? ReadTrack(*Model, Name) : FRawAnimSequenceTrack();
      FRawAnimSequenceTrack Track = Existing;
      if (!DecodeTrack(Pair.Value, Track, Sequence, OutError, CanonicalAssetPath)) {
        UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, TEXT("/bone_tracks/")
            + Pair.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"))); return false;
      }
      TSharedPtr<FJsonValue> Encoded;
      FTrackSource Source;
      Source.Path = TEXT("/bone_tracks/") + UE::Json::FJsonPointer::EscapePart(Pair.Key);
      Source.KeyCounts = FIntVector(Track.PosKeys.Num(), Track.RotKeys.Num(), Track.ScaleKeys.Num());
      if (bRecordTrackIntent) {
        // Freeze the decoded input before constant-key expansion. The sample
        // check below independently verifies the expanded native arguments.
        UeAgentAuthoringEncodedTypes::FScope Types;
        Encoded = EncodeTrack(Track, Sequence);
        Source.Value = Encoded;
        if (const auto Definition = Types.BuildDefinition(Encoded))
          Source.Definition = MakeShared<FJsonValueObject>(Definition);
      } else {
        Encoded = EncodeTrack(Track, Sequence);
      }
      TrackSources.Add(Name, MoveTemp(Source));
      if (!bExisting || !FJsonValue::CompareEqual(*EncodeTrack(Existing, Sequence), *Encoded)) {
        const int32 Count = FMath::Max3(Track.PosKeys.Num(), Track.RotKeys.Num(), Track.ScaleKeys.Num());
        if (Count == 0 || !ExpandConstant(Track.PosKeys, Count) ||
            !ExpandConstant(Track.RotKeys, Count) || !ExpandConstant(Track.ScaleKeys, Count) || Track.ContainsNaN()) {
          const auto& Counts = TrackSources.FindChecked(Name).KeyCounts;
          const auto Actual = MakeShared<FJsonObject>();
          Actual->SetNumberField(TEXT("position_keys"), Counts.X);
          Actual->SetNumberField(TEXT("rotation_keys"), Counts.Y);
          Actual->SetNumberField(TEXT("scale_keys"), Counts.Z);
          Actual->SetBoolField(TEXT("finite"), !Track.ContainsNaN());
          const auto Expected = MakeShared<FJsonObject>();
          Expected->SetStringField(TEXT("keys_per_channel"), Count > 0
              ? FString::Printf(TEXT("1 or %d"), Count) : TEXT("at least 1"));
          Expected->SetBoolField(TEXT("finite"), true);
          OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("animation_sequence_bone_keys_invalid"),
              TEXT("/bone_tracks/") + UE::Json::FJsonPointer::EscapePart(Pair.Key),
              MakeShared<FJsonValueObject>(Expected), MakeShared<FJsonValueObject>(Actual), TEXT("animation_apply"),
              TEXT("Decoded bone channels require finite values and either one key or the common sample count."));
          OutError.SourceKind = TEXT("related_input");
          OutError.SubjectId = Pair.Key;
          return false;
        }
        Changed.Add(Name);
      }
      Desired.Add(Name, MoveTemp(Track));
    }
  }

  const TSharedPtr<FJsonObject>* RequestedCurves = nullptr;
  TMap<FName, FRichCurve> DesiredCurves;
  TSet<FName> ChangedCurves;
  if (Document->HasField(TEXT("float_curves"))) {
    if (!Document->TryGetObjectField(TEXT("float_curves"), RequestedCurves) || !RequestedCurves) {
      OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("animation_float_curves_expected_object"), TEXT("/float_curves"),
          MakeShared<FJsonValueString>(TEXT("named curve map")), Document->TryGetField(TEXT("float_curves")), TEXT("curve_decode"), TEXT("float_curves must map each curve name to its curve object."));
      return false;
    }
    for (const auto& Pair : (*RequestedCurves)->Values) {
      const FName Name(*Pair.Key);
      if (Name.IsNone() || DesiredCurves.Contains(Name)) {
        OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("animation_curve_name_invalid"), TEXT("/float_curves/") + UE::Json::FJsonPointer::EscapePart(Pair.Key),
            MakeShared<FJsonValueString>(TEXT("unique non-None curve name")), MakeShared<FJsonValueString>(Pair.Key), TEXT("curve_decode"), TEXT("Curve names must be non-None and unique under native name comparison."));
        return false;
      }
      const FAnimationCurveIdentifier Id(Name, ERawCurveTrackTypes::RCT_Float);
      const FRichCurve* Existing = Model->FindRichCurve(Id);
      FRichCurve Curve = Existing ? *Existing : FRichCurve();
      if (!DecodeCurve(Pair.Value, Curve, TEXT("/float_curves/") + UE::Json::FJsonPointer::EscapePart(Pair.Key), true, OutError)) return false;
      if (!Existing || !(Curve == *Existing)) ChangedCurves.Add(Name);
      DesiredCurves.Add(Name, MoveTemp(Curve));
    }
  }

  const TSharedPtr<FJsonObject>* RequestedTransforms = nullptr;
  TSet<FName> DesiredTransforms;
  TMap<FAnimationCurveIdentifier, FRichCurve> ChangedTransformChannels;
  if (Document->HasField(TEXT("transform_curves"))) {
    if (!Document->TryGetObjectField(TEXT("transform_curves"), RequestedTransforms) || !RequestedTransforms) {
      OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("animation_transform_curves_expected_object"), TEXT("/transform_curves"),
          MakeShared<FJsonValueString>(TEXT("named transform-curve map")), Document->TryGetField(TEXT("transform_curves")), TEXT("curve_decode"), TEXT("transform_curves must map each curve name to its component curves."));
      return false;
    }
    for (const auto& Pair : (*RequestedTransforms)->Values) {
      const FName Name(*Pair.Key);
      const TSharedPtr<FJsonObject>* Channels = nullptr;
      if (Name.IsNone() || DesiredTransforms.Contains(Name) ||
          !Pair.Value || !Pair.Value->TryGetObject(Channels) || !Channels) {
        OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("animation_transform_curve_invalid"), TEXT("/transform_curves/") + UE::Json::FJsonPointer::EscapePart(Pair.Key),
            MakeShared<FJsonValueString>(TEXT("component object with a unique non-None curve name")), Pair.Value, TEXT("curve_decode"),
            FString::Printf(TEXT("Transform curve '%s' must have a unique non-None name and an object value."), *Pair.Key));
        return false;
      }
      DesiredTransforms.Add(Name);
      for (const auto& ChannelPair : (*Channels)->Values) {
        int32 Channel = INDEX_NONE;
        for (int32 Index = 0; Index < 3; ++Index)
          if (ChannelPair.Key == TransformChannels[Index]) Channel = Index;
        const TSharedPtr<FJsonObject>* Axes = nullptr;
        if (Channel == INDEX_NONE || !ChannelPair.Value || !ChannelPair.Value->TryGetObject(Axes) || !Axes) {
          OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("animation_transform_curve_channel_invalid"), TEXT("/transform_curves/")
              + UE::Json::FJsonPointer::EscapePart(Pair.Key) + TEXT("/") + UE::Json::FJsonPointer::EscapePart(ChannelPair.Key),
              MakeShared<FJsonValueString>(TEXT("position, rotation or scale with an axis object")), ChannelPair.Value, TEXT("curve_decode"),
              FString::Printf(TEXT("Transform component '%s' must be position, rotation or scale, containing named axis curves."), *ChannelPair.Key));
          return false;
        }
        for (const auto& AxisPair : (*Axes)->Values) {
          int32 Axis = INDEX_NONE;
          for (int32 Index = 0; Index < 3; ++Index)
            if (AxisPair.Key == VectorAxes[Index]) Axis = Index;
          if (Axis == INDEX_NONE) {
            OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("animation_transform_curve_axis_invalid"), TEXT("/transform_curves/")
                + UE::Json::FJsonPointer::EscapePart(Pair.Key) + TEXT("/") + UE::Json::FJsonPointer::EscapePart(ChannelPair.Key) + TEXT("/") + UE::Json::FJsonPointer::EscapePart(AxisPair.Key),
                MakeShared<FJsonValueString>(TEXT("x, y or z")), MakeShared<FJsonValueString>(AxisPair.Key), TEXT("curve_decode"), TEXT("Transform-curve axis must be x, y or z."));
            return false;
          }
          const FAnimationCurveIdentifier Id = TransformChannel(Name, Channel, Axis);
          const FRichCurve* Existing = Model->FindRichCurve(Id);
          FRichCurve Curve = Existing ? *Existing : FRichCurve();
          if (!DecodeCurve(AxisPair.Value, Curve, TEXT("/transform_curves/") + UE::Json::FJsonPointer::EscapePart(Pair.Key)
              + TEXT("/") + UE::Json::FJsonPointer::EscapePart(ChannelPair.Key) + TEXT("/") + UE::Json::FJsonPointer::EscapePart(AxisPair.Key), false, OutError)) return false;
          if (!Existing || !(Curve == *Existing)) ChangedTransformChannels.Add(Id, MoveTemp(Curve));
        }
      }
    }
  }

  // Declared before the bracket so all exits settle after BracketClosed, including
  // a controller rejection following an earlier successful change.
  ON_SCOPE_EXIT { Sequence.WaitOnExistingCompression(); };
  IAnimationDataController& Controller = Sequence.GetController();
  IAnimationDataController::FScopedBracket Bracket(Controller,
      FText::FromString(TEXT("Edit animation sequence HJSON")), false);
  if (FrameRate != Model->GetFrameRate()) {
    Controller.SetFrameRate(FrameRate, false);
    if (Model->GetFrameRate() != FrameRate) {
      OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_sequence_frame_rate_not_retained"), TEXT("The native model did not retain the requested frame rate."), TEXT("animation_apply"), FString::Printf(TEXT("expected=%d/%d:actual=%d/%d"),
          FrameRate.Numerator, FrameRate.Denominator,
          Model->GetFrameRate().Numerator, Model->GetFrameRate().Denominator));
      OutError.ActualValue = FrameRateValue(Model->GetFrameRate());
      OutError.ExpectedValue = FrameRateValue(FrameRate);
      OutError.SourceKind = Document->HasField(TEXT("frame_rate")) ? TEXT("related_input") : TEXT("native_result");
      UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, TEXT("/frame_rate"));
      return false;
    }
    ++OutChanges;
  }
  if (Frames != Model->GetNumberOfFrames()) {
    Controller.SetNumberOfFrames(FFrameNumber(Frames), false);
    if (Model->GetNumberOfFrames() != Frames) {
      OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_sequence_frame_count_not_retained"), TEXT("The native model did not retain the requested frame count."), TEXT("animation_apply"), FString::Printf(TEXT("expected=%d:actual=%d"),
          Frames, Model->GetNumberOfFrames()));
      OutError.ActualValue = MakeShared<FJsonValueNumber>(Model->GetNumberOfFrames());
      OutError.ExpectedValue = MakeShared<FJsonValueNumber>(Frames);
      OutError.SourceKind = Document->HasField(TEXT("number_of_frames")) ? TEXT("related_input") : TEXT("native_result");
      UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, TEXT("/number_of_frames"));
      return false;
    }
    ++OutChanges;
  }
  if (RequestedTracks) {
    TArray<FName> ExistingNames;
    Model->GetBoneTrackNames(ExistingNames);
    for (FName Name : ExistingNames) {
      if (!Desired.Contains(Name)) {
        if (!Controller.RemoveBoneTrack(Name, false)) {
          OutError = ControllerFailure(TEXT("animation_sequence_remove_bone_track_failed"),
              TEXT("The native animation controller did not remove the bone track."), Name, TEXT("remove_bone_track"));
          return false;
        }
        ++OutChanges;
      }
    }
    for (FName Name : Changed) {
      const FRawAnimSequenceTrack& Track = Desired.FindChecked(Name);
      const bool bTrackReady = Model->IsValidBoneTrackName(Name) || Controller.AddBoneCurve(Name, false);
      const bool bKeysWritten = bTrackReady && Controller.SetBoneTrackKeys(Name, Track.PosKeys, Track.RotKeys, Track.ScaleKeys, false);
      if (!bKeysWritten) {
        OutError = ControllerFailure(TEXT("animation_sequence_set_bone_track_failed"),
            bTrackReady ? TEXT("The native animation controller did not write the bone track keys.")
                : TEXT("The native animation controller did not add the requested bone track."),
            Name, bTrackReady ? TEXT("set_bone_track_keys") : TEXT("add_bone_track"));
        return false;
      }
      // The Sequencer controller can report an inner write error yet return true.
      // Read authored sample frames, allowing native constant compression and
      // quaternion/Euler representation changes instead of comparing raw arrays.
      for (int32 Index = 0; Index < Track.PosKeys.Num(); ++Index) {
        const FTransform3f Expected(Track.RotKeys[Index], Track.PosKeys[Index], Track.ScaleKeys[Index]);
        const FTransform3f Actual(Model->GetBoneTrackTransform(Name, FFrameNumber(Index)));
        // Keep the native sample oracle, including quaternion sign equivalence.
        // Each failed channel supplies its own facts; no transform is re-compared as JSON.
        const bool bTranslation = Actual.TranslationEquals(Expected, UE_KINDA_SMALL_NUMBER);
        const bool bRotation = Actual.RotationEquals(Expected, UE_KINDA_SMALL_NUMBER);
        const bool bScale = Actual.Scale3DEquals(Expected, UE_KINDA_SMALL_NUMBER);
        if (!bTranslation || !bRotation || !bScale) {
          const auto& Source = TrackSources.FindChecked(Name);
          const TCHAR* Channel = !bTranslation ? TEXT("PosKeys") : !bRotation ? TEXT("RotKeys") : TEXT("ScaleKeys");
          const int32 Count = !bTranslation ? Source.KeyCounts.X : !bRotation ? Source.KeyCounts.Y : Source.KeyCounts.Z;
          const int32 AuthoredIndex = Count == 1 ? 0 : Index;
          OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_sequence_bone_key_not_retained"),
              FString::Printf(TEXT("Bone %s sample %d did not retain %s."), *Name.ToString(), Index, Channel), TEXT("animation_apply"));
          OutError.SubjectId = Name.ToString();
          OutError.SourceKind = TEXT("related_input");
          OutError.Source.JsonPath = Source.Path + FString::Printf(TEXT("/fields/%s/items/%d"), Channel, AuthoredIndex);
          OutError.ExpectedValuePresent = true;
          OutError.ActualValuePresent = true;
          if (bTranslation && !bRotation) {
            const FQuat4f ExpectedRotation = Expected.GetRotation(), ActualRotation = Actual.GetRotation();
            OutError.ExpectedValue = UeAgentPropertyValueCodec::EncodeStructValue(
                TVariantStructure<FQuat4f>::Get(), &ExpectedRotation, &Sequence, FCodecOptions());
            OutError.ActualValue = UeAgentPropertyValueCodec::EncodeStructValue(
                TVariantStructure<FQuat4f>::Get(), &ActualRotation, &Sequence, FCodecOptions());
          } else {
            const FVector3f ExpectedVector = !bTranslation ? Expected.GetTranslation() : Expected.GetScale3D();
            const FVector3f ActualVector = !bTranslation ? Actual.GetTranslation() : Actual.GetScale3D();
            for (int32 Component = 0; Component < 3; ++Component) {
              if (FMath::Abs(ExpectedVector[Component] - ActualVector[Component]) <= UE_KINDA_SMALL_NUMBER) continue;
              OutError.Source.JsonPath += FString::Printf(TEXT("/components/%d"), Component);
              OutError.ExpectedValue = MakeShared<FJsonValueNumber>(ExpectedVector[Component]);
              OutError.ActualValue = MakeShared<FJsonValueNumber>(ActualVector[Component]);
              break;
            }
          }
          return false;
        }
      }
      if (const auto* Source = TrackSources.Find(Name); bRecordTrackIntent && Source) {
        // Only the isolated proposal records this proof. It captures the actual
        // controller's constant-channel reduction and Euler/quaternion storage;
        // final readback must match that frozen result, not a widened tolerance.
        const auto Type = UeAgentAuthoringTypeDescriptors::DescribeStruct(FRawAnimSequenceTrack::StaticStruct());
        if (!UeAgentNativeNormalizationIntent::RecordValue(Source->Path, Source->Value,
              EncodeTrack(ReadTrack(*Model, Name), Sequence), OutError, &Type, Source->Definition)) {
          OutError.SemanticPointer = Source->Path;
          OutError.Source.JsonPath = Source->Path;
          UeAgentAuthoringDiagnostics::ReportConsistencyWarning(FString(), OutError, TEXT("native_normalization_observation"));
          OutError.Reset();
        }
      }
      ++OutChanges;
    }
  }
  if (RequestedCurves) {
    TArray<FName> Removed;
    for (const FFloatCurve& Curve : Model->GetFloatCurves())
      if (!DesiredCurves.Contains(Curve.GetName())) Removed.Add(Curve.GetName());
    for (FName Name : Removed) {
      if (!Controller.RemoveCurve(FAnimationCurveIdentifier(Name, ERawCurveTrackTypes::RCT_Float), false)) {
        OutError = ControllerFailure(TEXT("animation_curve_remove_failed"),
            TEXT("The native animation controller did not remove the float curve."), Name, TEXT("remove_float_curve"));
        return false;
      }
      ++OutChanges;
    }
    for (FName Name : ChangedCurves) {
      const FAnimationCurveIdentifier Id(Name, ERawCurveTrackTypes::RCT_Float);
      if (!Model->FindRichCurve(Id) && !Controller.AddCurve(Id, AACF_Editable, false)) {
        OutError = ControllerFailure(TEXT("animation_curve_add_failed"),
            TEXT("The native animation controller did not add the float curve."), Name, TEXT("add_float_curve"));
        return false;
      }
      if (!WriteCurve(Controller, Id, DesiredCurves.FindChecked(Name),
          TEXT("/float_curves/") + UE::Json::FJsonPointer::EscapePart(Name.ToString()), OutError)) return false;
      ++OutChanges;
    }
  }
  if (RequestedTransforms) {
    TArray<FName> Removed;
    for (const FTransformCurve& Curve : Model->GetTransformCurves())
      if (!DesiredTransforms.Contains(Curve.GetName())) Removed.Add(Curve.GetName());
    for (FName Name : Removed) {
      if (!Controller.RemoveCurve(FAnimationCurveIdentifier(Name, ERawCurveTrackTypes::RCT_Transform), false)) {
        OutError = ControllerFailure(TEXT("animation_transform_curve_remove_failed"),
            TEXT("The native animation controller did not remove the transform curve."), Name, TEXT("remove_transform_curve"));
        return false;
      }
      ++OutChanges;
    }
    for (FName Name : DesiredTransforms) {
      const FAnimationCurveIdentifier Id(Name, ERawCurveTrackTypes::RCT_Transform);
      if (!Model->FindTransformCurve(Id)) {
        if (!Controller.AddCurve(Id, AACF_DriveTrack | AACF_Editable, false)) {
          OutError = ControllerFailure(TEXT("animation_transform_curve_add_failed"),
              TEXT("The native animation controller did not add the transform curve."), Name, TEXT("add_transform_curve"));
          return false;
        }
        ++OutChanges;
      }
    }
    for (const auto& Pair : ChangedTransformChannels) {
      const FString Path = TEXT("/transform_curves/") + UE::Json::FJsonPointer::EscapePart(Pair.Key.CurveName.ToString())
          + TEXT("/") + TransformChannels[int32(Pair.Key.Channel)] + TEXT("/") + VectorAxes[int32(Pair.Key.Axis)];
      if (!WriteCurve(Controller, Pair.Key, Pair.Value, Path, OutError)) return false;
      ++OutChanges;
    }
  }
  if (!ApplyAnimationEvents(Sequence, *Document, OutChanges, OutError, false, nullptr, CanonicalAssetPath)) return false;
  if (Document->HasField(TEXT("metadata"))) {
    const TSharedPtr<FJsonObject>* Metadata = nullptr;
    if (!Document->TryGetObjectField(TEXT("metadata"), Metadata) || !Metadata) {
      OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("animation_metadata_expected_object"),
          TEXT("/metadata"), MakeShared<FJsonValueString>(TEXT("object")), Document->TryGetField(TEXT("metadata")),
          TEXT("animation_apply"), TEXT("Animation metadata must be an object.")); return false;
    }
    if (!UeAgentAuthoringObjectMetadata::ApplyDesired(Sequence, **Metadata, OutError)) {
      UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, TEXT("/metadata"));
      return false;
    }
  }
  return true;
}
}
