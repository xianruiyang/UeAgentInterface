#include "Utilities/AuthoringCore/UeAgentAnimationSequenceChanges.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Utilities/AuthoringCore/UeAgentTimelineValueJson.h"
#include "Utilities/AuthoringCore/UeAgentMovieSceneAuthoring.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringObjectMetadata.h"
#include "Animation/AnimData/CurveIdentifier.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimSequence.h"
#include "UObject/Package.h"
namespace UE::UAI::Authoring::Timeline {
using namespace Json;

		bool TryPointSeconds(const FTimelineAnimationPoint& Point,
			double& OutSeconds, FUeAgentTimelineAnimationExecutionError& OutError)
		{
			FFrameRate Rate;
			int64 Tick = 0;
			if (!TryRate(Point.Rate, Rate, OutError)) return PrefixChildFailure(nullptr, TEXT("/rate"), OutError);
			if (!StateSemantic::TryParseInt64Decimal(Point.Tick, Tick))
				return ValueFailure(nullptr, TEXT("/tick"), TEXT("timeline_animation_native_tick_invalid"),
					TEXT("tick representable as int64"), MakeShared<FJsonValueString>(Point.Tick), OutError);
			OutSeconds = static_cast<double>(Tick)
				* static_cast<double>(Rate.Denominator)
				/ static_cast<double>(Rate.Numerator);
			return FMath::IsFinite(OutSeconds) ? true : Fail(OutError, UE::UAI::Contract::FDiagnostic::Failure(TEXT("timeline_animation_native_time_invalid"), TEXT("time must resolve to finite seconds"), TEXT("native_execution")), Point.ClockId);
		}

	bool FTimelineAnimationCurveKey::TryValidate(FUeAgentTimelineAnimationExecutionError& Error) const
	{
		Error = {};
		if (!Time.TryValidate(Error)) return PrefixChildFailure(nullptr, TEXT("/time"), Error);
		float NativeValue = 0;
		if (!UeAgentPropertyValueCodec::TryConvertFloat32(Value, NativeValue, Error))
		{
			if (Error.Source.SourceFile.IsEmpty()) Error.SemanticPointer = TEXT("/value");
			return false;
		}
		return Interpolation == TEXT("constant") || Interpolation == TEXT("linear") || Interpolation == TEXT("cubic")
			|| ValueFailure(nullptr, TEXT("/interpolation"), TEXT("timeline_animation_curve_key_invalid"),
				TEXT("constant, linear, or cubic"), MakeShared<FJsonValueString>(Interpolation), Error);
	}

	bool FTimelineAnimationCurveChange::TryValidate(FUeAgentTimelineAnimationExecutionError& Error) const
	{
		Error = {};
		if (CurveRef.IsEmpty()) return ValueFailure(nullptr, TEXT("/curve_ref"), TEXT("timeline_animation_curve_change_invalid"),
			TEXT("a non-empty curve reference"), MakeShared<FJsonValueString>(CurveRef), Error);
		if (Operation != TEXT("upsert") && Operation != TEXT("remove"))
			return ValueFailure(nullptr, TEXT("/operation"), TEXT("timeline_animation_curve_change_invalid"),
				TEXT("upsert or remove"), MakeShared<FJsonValueString>(Operation), Error);
		for (int32 Index = 0; Index < Keys.Num(); ++Index)
			if (!Keys[Index].TryValidate(Error)) return PrefixChildFailure(nullptr, TEXT("/keys/") + FString::FromInt(Index), Error);
		return true;
	}

	bool FTimelineAnimationMetadataChange::TryValidate(FUeAgentTimelineAnimationExecutionError& Error) const
	{
		Error = {};
		if (MetadataRef.IsEmpty()) return ValueFailure(nullptr, TEXT("/metadata_ref"), TEXT("timeline_animation_metadata_change_invalid"),
			TEXT("a non-empty metadata reference"), MakeShared<FJsonValueString>(MetadataRef), Error);
		if (Operation != TEXT("upsert") && Operation != TEXT("remove"))
			return ValueFailure(nullptr, TEXT("/operation"), TEXT("timeline_animation_metadata_change_invalid"),
				TEXT("upsert or remove"), MakeShared<FJsonValueString>(Operation), Error);
		return ValueType == TEXT("string") || ValueType == TEXT("integer") || ValueType == TEXT("decimal")
			|| ValueType == TEXT("boolean") || ValueType == TEXT("asset_ref")
			|| ValueFailure(nullptr, TEXT("/value_type"), TEXT("timeline_animation_metadata_change_invalid"),
				TEXT("string, integer, decimal, boolean, or asset_ref"), MakeShared<FJsonValueString>(ValueType), Error);
	}

	bool FTimelineAnimationEventChange::TryValidate(FUeAgentTimelineAnimationExecutionError& Error) const
	{
		Error = {};
		if (EventRef.IsEmpty()) return ValueFailure(nullptr, TEXT("/event_ref"), TEXT("timeline_animation_event_change_invalid"),
			TEXT("a non-empty event reference"), MakeShared<FJsonValueString>(EventRef), Error);
		if (Operation != TEXT("upsert") && Operation != TEXT("remove"))
			return ValueFailure(nullptr, TEXT("/operation"), TEXT("timeline_animation_event_change_invalid"),
				TEXT("upsert or remove"), MakeShared<FJsonValueString>(Operation), Error);
		if (EventKind != TEXT("notify_track") && EventKind != TEXT("notify") && EventKind != TEXT("notify_state") && EventKind != TEXT("sync_marker"))
			return ValueFailure(nullptr, TEXT("/event_kind"), TEXT("timeline_animation_event_change_invalid"),
				TEXT("notify_track, notify, notify_state, or sync_marker"), MakeShared<FJsonValueString>(EventKind), Error);
		if (Time.IsSet() && !Time->TryValidate(Error)) return PrefixChildFailure(nullptr, TEXT("/time"), Error);
		if (Range.IsSet() && !Range->TryValidate(Error)) return PrefixChildFailure(nullptr, TEXT("/range"), Error);
		return true;
	}

	bool FTimelineAnimationCurveKey::IsValid() const { FUeAgentTimelineAnimationExecutionError Error; return TryValidate(Error); }
	bool FTimelineAnimationCurveChange::IsValid() const { FUeAgentTimelineAnimationExecutionError Error; return TryValidate(Error); }
	bool FTimelineAnimationMetadataChange::IsValid() const { FUeAgentTimelineAnimationExecutionError Error; return TryValidate(Error); }
	bool FTimelineAnimationEventChange::IsValid() const { FUeAgentTimelineAnimationExecutionError Error; return TryValidate(Error); }
		bool ParseCurveKey(const TSharedPtr<FJsonObject>& Root,
			FTimelineAnimationCurveKey& Out,
			FUeAgentTimelineAnimationExecutionError& Error)
		{
			TSharedPtr<FJsonObject> Time;
			if (!RequiredObject(Root, TEXT("time"), Time, Error)) return false;
			if (!ParsePoint(Time, Out.Time, Error)) return PrefixChildFailure(Root, TEXT("/time"), Error);
			if (!RequiredNumber(Root, TEXT("value"), Out.Value, Error)
				|| !RequiredString(Root, TEXT("interpolation"), Out.Interpolation, Error)) return false;
			float NativeValue = 0;
			if (!UeAgentPropertyValueCodec::TryConvertFloat32(Out.Value, NativeValue, Error))
			{
				Error.SemanticPointer = TEXT("/value");
				UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Root, TEXT("value"));
				return false;
			}
			return Out.Interpolation == TEXT("constant") || Out.Interpolation == TEXT("linear") || Out.Interpolation == TEXT("cubic")
				|| FieldFailure(Root, TEXT("interpolation"), TEXT("timeline_animation_curve_key_invalid"), TEXT("constant, linear or cubic"), Error);
		}

		bool ParseCurveChange(const TSharedPtr<FJsonObject>& Root,
			FTimelineAnimationCurveChange& Out,
			FUeAgentTimelineAnimationExecutionError& Error)
		{
			if (!RequiredString(Root, TEXT("curve_ref"), Out.CurveRef, Error)
				|| !RequiredString(Root, TEXT("operation"), Out.Operation, Error)
				|| !ParseObjectArray(Root, TEXT("keys"), Out.Keys, true, ParseCurveKey, Error)) return false;
			if (Out.CurveRef.IsEmpty()) return FieldFailure(Root, TEXT("curve_ref"), TEXT("timeline_animation_curve_change_invalid"), TEXT("a nonempty curve reference"), Error);
			return Out.Operation == TEXT("upsert") || Out.Operation == TEXT("remove")
				|| FieldFailure(Root, TEXT("operation"), TEXT("timeline_animation_curve_change_invalid"), TEXT("upsert or remove"), Error);
		}

		bool ParseMetadataChange(const TSharedPtr<FJsonObject>& Root,
			FTimelineAnimationMetadataChange& Out,
			FUeAgentTimelineAnimationExecutionError& Error)
		{
			if (!RequiredString(Root, TEXT("metadata_ref"), Out.MetadataRef, Error)
				|| !RequiredString(Root, TEXT("operation"), Out.Operation, Error)
				|| !RequiredString(Root, TEXT("value_type"), Out.ValueType, Error)
				|| !RequiredString(Root, TEXT("value_lexeme"), Out.ValueLexeme, Error)) return false;
			if (Out.MetadataRef.IsEmpty()) return FieldFailure(Root, TEXT("metadata_ref"), TEXT("timeline_animation_metadata_change_invalid"), TEXT("a nonempty metadata reference"), Error);
			if (Out.Operation != TEXT("upsert") && Out.Operation != TEXT("remove"))
				return FieldFailure(Root, TEXT("operation"), TEXT("timeline_animation_metadata_change_invalid"), TEXT("upsert or remove"), Error);
			return Out.ValueType == TEXT("string") || Out.ValueType == TEXT("integer") || Out.ValueType == TEXT("decimal")
				|| Out.ValueType == TEXT("boolean") || Out.ValueType == TEXT("asset_ref")
				|| FieldFailure(Root, TEXT("value_type"), TEXT("timeline_animation_metadata_change_invalid"), TEXT("string, integer, decimal, boolean or asset_ref"), Error);
		}

		bool ParseEventChange(const TSharedPtr<FJsonObject>& Root,
			FTimelineAnimationEventChange& Out,
			FUeAgentTimelineAnimationExecutionError& Error)
		{
			if (!RequiredString(Root, TEXT("event_ref"), Out.EventRef, Error)
				|| !RequiredString(Root, TEXT("event_kind"), Out.EventKind, Error)
				|| !RequiredString(Root, TEXT("operation"), Out.Operation, Error)
				|| !OptionalString(Root, TEXT("payload_class_ref"), Out.PayloadClassRef, Error)
				|| !OptionalString(Root, TEXT("track_ref"), Out.TrackRef, Error))
			{
				return false;
			}
			TSharedPtr<FJsonObject> Object;
			bool bPresent = false;
			if (!OptionalObject(Root, TEXT("time"), Object, bPresent, Error))
			{
				return false;
			}
			if (bPresent)
			{
				FTimelineAnimationPoint Point;
				if (!ParsePoint(Object, Point, Error)) return PrefixChildFailure(Root, TEXT("/time"), Error);
				Out.Time = MoveTemp(Point);
			}
			if (!OptionalObject(Root, TEXT("range"), Object, bPresent, Error))
			{
				return false;
			}
			if (bPresent)
			{
				FTimelineAnimationRange Range;
				if (!ParseRange(Object, Range, Error)) return PrefixChildFailure(Root, TEXT("/range"), Error);
				Out.Range = MoveTemp(Range);
			}
			if (Out.EventRef.IsEmpty()) return FieldFailure(Root, TEXT("event_ref"), TEXT("timeline_animation_event_change_invalid"), TEXT("a nonempty event reference"), Error);
			if (Out.Operation != TEXT("upsert") && Out.Operation != TEXT("remove"))
				return FieldFailure(Root, TEXT("operation"), TEXT("timeline_animation_event_change_invalid"), TEXT("upsert or remove"), Error);
			return Out.EventKind == TEXT("notify_track") || Out.EventKind == TEXT("notify") || Out.EventKind == TEXT("notify_state") || Out.EventKind == TEXT("sync_marker")
				|| FieldFailure(Root, TEXT("event_kind"), TEXT("timeline_animation_event_change_invalid"), TEXT("notify_track, notify, notify_state or sync_marker"), Error);
		}

		bool ApplySequenceCurves(UAnimSequence& Sequence,
			const FSequenceChanges& Data,
			FUeAgentTimelineAnimationExecutionError& OutError, const FString& CanonicalObjectPath)
		{
			IAnimationDataController& Controller = Sequence.GetController();
			const IAnimationDataModel* Model = Sequence.GetDataModel();
			const FFrameRate NativeRate = Model ? Model->GetFrameRate() : FFrameRate(0, 0);
			const FString NativeClock = TEXT("animation_sequence:") + (CanonicalObjectPath.IsEmpty() ? Sequence.GetPathName() : CanonicalObjectPath);
			Controller.OpenBracket(FText::FromString(TEXT("UAI update curves")), false);
			bool bOk = true;
			for (int32 CurveIndex = 0; CurveIndex < Data.CurveChanges.Num(); ++CurveIndex)
			{
				const auto& Change = Data.CurveChanges[CurveIndex];
				const FAnimationCurveIdentifier Id(FName(*Change.CurveRef), ERawCurveTrackTypes::RCT_Float);
				if (Change.Operation == TEXT("remove"))
				{
					Controller.RemoveCurve(Id, false);
					continue;
				}
				Controller.AddCurve(Id, AACF_Editable, false);
				TArray<FRichCurveKey> Keys;
				for (int32 KeyIndex = 0; KeyIndex < Change.Keys.Num(); ++KeyIndex)
				{
					const auto& Source = Change.Keys[KeyIndex];
					const FString KeyPointer = FString::Printf(TEXT("/curve_changes/%d/keys/%d"), CurveIndex, KeyIndex);
					int64 Tick = 0;
					double Seconds = 0.0;
					if (!TryPointTick(Source.Time, NativeClock, NativeRate, Tick, OutError)
						|| !TryPointSeconds(Source.Time, Seconds, OutError)) {
						PrefixChildFailure(Data.SourceRecord, KeyPointer + TEXT("/time"), OutError);
						OutError.CandidateName = Change.CurveRef; bOk = false; break;
					}
					float NativeSeconds = 0, NativeValue = 0;
					UE::UAI::Contract::FDiagnostic ValueError;
					const bool bTimeValid = UeAgentPropertyValueCodec::TryConvertFloat32(Seconds, NativeSeconds, ValueError);
					if (!bTimeValid || !UeAgentPropertyValueCodec::TryConvertFloat32(Source.Value, NativeValue, ValueError)) {
						OutError = ValueError;
						PrefixChildFailure(Data.SourceRecord, KeyPointer + (bTimeValid ? TEXT("/value") : TEXT("/time")), OutError);
						OutError.CandidateName = Change.CurveRef;
						bOk = false; break;
					}
					FRichCurveKey Key(NativeSeconds, NativeValue);
					Key.InterpMode = Source.Interpolation == TEXT("constant")
						? RCIM_Constant : Source.Interpolation == TEXT("linear")
							? RCIM_Linear : RCIM_Cubic;
					Keys.Add(Key);
				}
				if (!bOk || !Controller.SetCurveKeys(Id, Keys, false))
				{
					bOk = false;
					if (!OutError.IsValid()) {
						OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("timeline_animation_curve_keys_failed"), TEXT("The animation data controller rejected the replacement curve keys."), TEXT("native_execution"));
						OutError.CandidateName = Change.CurveRef;
						OutError.TargetPath = CanonicalObjectPath;
						OutError.ResourceDimension = TEXT("curve_keys");
						OutError.ResourceObserved = Keys.Num();
						OutError.SemanticPointer = FString::Printf(TEXT("/curve_changes/%d/keys"), CurveIndex);
						if (Data.SourceRecord) UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Data.SourceRecord);
						OutError.SourceKind = TEXT("native_result");
					}
					break;
				}
			}
			Controller.CloseBracket(false);
			return bOk;
		}

		bool ApplySequenceMetadata(UAnimSequence& Sequence,
			const FSequenceChanges& Data,
			FUeAgentTimelineAnimationExecutionError& OutError, const FString& CanonicalObjectPath)
		{
			UPackage* Package = Sequence.GetOutermost();
			if (!Package) {
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("timeline_animation_metadata_owner_unavailable"), TEXT("The animation sequence has no package to hold metadata."), TEXT("native_execution"), Sequence.GetPathName());
				OutError.SourceKind = TEXT("native_result");
				OutError.TargetPath = CanonicalObjectPath;
				return false;
			}
			TMap<FName, TOptional<FString>> Changes;
			for (const auto& Change : Data.MetadataChanges)
			{
				if (Change.Operation == TEXT("remove"))
					Changes.Add(FName(*Change.MetadataRef), TOptional<FString>());
				else Changes.Add(FName(*Change.MetadataRef), Change.ValueLexeme);
			}
			UeAgentAuthoringObjectMetadata::ApplyChanges(Sequence, Changes);
			return true;
		}

bool ParseSequenceChanges(const TSharedPtr<FJsonObject>& Root, FSequenceChanges& Out, FUeAgentTimelineAnimationExecutionError& Error) {
 Out = {};
 Out.SourceRecord = Root;
 return (!Root->HasField(TEXT("curve_changes")) || ParseObjectArray(Root, TEXT("curve_changes"), Out.CurveChanges, true, ParseCurveChange, Error))
   && (!Root->HasField(TEXT("metadata_changes")) || ParseObjectArray(Root, TEXT("metadata_changes"), Out.MetadataChanges, true, ParseMetadataChange, Error))
   && (!Root->HasField(TEXT("event_changes")) || ParseObjectArray(Root, TEXT("event_changes"), Out.EventChanges, true, ParseEventChange, Error));
}
}
