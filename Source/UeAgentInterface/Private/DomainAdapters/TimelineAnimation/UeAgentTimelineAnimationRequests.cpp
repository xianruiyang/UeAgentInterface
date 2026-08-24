#include "DomainAdapters/TimelineAnimation/UeAgentTimelineAnimationRequests.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Core/StateSemantic/UeAgentTemporalContract.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::DomainAdapters::TimelineAnimation
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::PlanningPreflight;
	using namespace UE::UAI::StateSemantic;

	namespace
	{
		bool Fail(
			FUeAgentTimelineAnimationExecutionError& OutError,
			FString Code,
			FString SubjectId = FString(),
			FString Detail = FString())
		{
			OutError = {};
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(SubjectId);
			OutError.Detail = MoveTemp(Detail);
			return false;
		}

		bool RequiredObject(
			const TSharedPtr<FJsonObject>& Root,
			const TCHAR* Field,
			TSharedPtr<FJsonObject>& Out,
			FUeAgentTimelineAnimationExecutionError& Error)
		{
			const TSharedPtr<FJsonObject>* Found = nullptr;
			if (!Root.IsValid() || !Root->TryGetObjectField(Field, Found)
				|| !Found || !Found->IsValid())
			{
				return Fail(Error, TEXT("timeline_animation_field_object_missing"), Field);
			}
			Out = *Found;
			return true;
		}

		bool OptionalObject(
			const TSharedPtr<FJsonObject>& Root,
			const TCHAR* Field,
			TSharedPtr<FJsonObject>& Out,
			bool& bPresent,
			FUeAgentTimelineAnimationExecutionError& Error)
		{
			Out.Reset();
			bPresent = Root.IsValid() && Root->HasField(Field);
			return !bPresent || RequiredObject(Root, Field, Out, Error);
		}

		bool RequiredString(
			const TSharedPtr<FJsonObject>& Root,
			const TCHAR* Field,
			FString& Out,
			FUeAgentTimelineAnimationExecutionError& Error)
		{
			if (!Root.IsValid() || !Root->TryGetStringField(Field, Out))
			{
				return Fail(Error, TEXT("timeline_animation_field_string_missing"), Field);
			}
			return true;
		}

		bool OptionalString(
			const TSharedPtr<FJsonObject>& Root,
			const TCHAR* Field,
			FString& Out,
			FUeAgentTimelineAnimationExecutionError& Error)
		{
			Out.Reset();
			return !Root->HasField(Field) || RequiredString(Root, Field, Out, Error);
		}

		bool OptionalBool(
			const TSharedPtr<FJsonObject>& Root,
			const TCHAR* Field,
			TOptional<bool>& Out,
			FUeAgentTimelineAnimationExecutionError& Error)
		{
			Out.Reset();
			if (!Root->HasField(Field))
			{
				return true;
			}
			bool Value = false;
			if (!Root->TryGetBoolField(Field, Value))
			{
				return Fail(Error, TEXT("timeline_animation_field_boolean_invalid"), Field);
			}
			Out = Value;
			return true;
		}

		bool OptionalNumber(
			const TSharedPtr<FJsonObject>& Root,
			const TCHAR* Field,
			TOptional<double>& Out,
			FUeAgentTimelineAnimationExecutionError& Error)
		{
			Out.Reset();
			if (!Root->HasField(Field))
			{
				return true;
			}
			double Value = 0.0;
			if (!Root->TryGetNumberField(Field, Value) || !FMath::IsFinite(Value))
			{
				return Fail(Error, TEXT("timeline_animation_field_number_invalid"), Field);
			}
			Out = Value;
			return true;
		}

		bool ParseStringArray(
			const TSharedPtr<FJsonObject>& Root,
			const TCHAR* Field,
			TArray<FString>& Out,
			bool bRequired,
			FUeAgentTimelineAnimationExecutionError& Error)
		{
			Out.Reset();
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Root->TryGetArrayField(Field, Values) || !Values)
			{
				return !bRequired || Fail(
					Error, TEXT("timeline_animation_field_array_missing"), Field);
			}
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				FString Text;
				if (!Value.IsValid() || !Value->TryGetString(Text))
				{
					return Fail(
						Error, TEXT("timeline_animation_array_string_invalid"), Field);
				}
				Out.Add(MoveTemp(Text));
			}
			return true;
		}

		bool ParseRevision(
			const TSharedPtr<FJsonObject>& Root,
			FTimelineAnimationRevision& Out,
			FUeAgentTimelineAnimationExecutionError& Error)
		{
			Out = {};
			FString SequenceLexeme;
			return RequiredString(Root, TEXT("source_id"), Out.SourceId, Error)
				&& RequiredString(Root, TEXT("generation"), Out.Generation, Error)
				&& RequiredString(Root, TEXT("sequence"), SequenceLexeme, Error)
				&& LexTryParseString(Out.Sequence, *SequenceLexeme)
				&& FString::Printf(TEXT("%lld"), Out.Sequence) == SequenceLexeme
				&& (Out.IsValid() || Fail(
					Error, TEXT("timeline_animation_revision_invalid"), Out.SourceId));
		}

		bool ParseRate(
			const TSharedPtr<FJsonObject>& Root,
			FTimelineAnimationRate& Out,
			FUeAgentTimelineAnimationExecutionError& Error)
		{
			Out = {};
			return RequiredString(Root, TEXT("numerator"), Out.Numerator, Error)
				&& RequiredString(Root, TEXT("denominator"), Out.Denominator, Error)
				&& (Out.IsValid() || Fail(
					Error, TEXT("timeline_animation_rate_invalid"), Out.Numerator));
		}

		bool ParsePoint(
			const TSharedPtr<FJsonObject>& Root,
			FTimelineAnimationPoint& Out,
			FUeAgentTimelineAnimationExecutionError& Error)
		{
			Out = {};
			TSharedPtr<FJsonObject> Rate;
			return RequiredString(Root, TEXT("clock_id"), Out.ClockId, Error)
				&& RequiredString(Root, TEXT("tick"), Out.Tick, Error)
				&& RequiredObject(Root, TEXT("rate"), Rate, Error)
				&& ParseRate(Rate, Out.Rate, Error)
				&& RequiredString(Root, TEXT("state"), Out.State, Error)
				&& (Out.IsValid() || Fail(
					Error, TEXT("timeline_animation_point_invalid"), Out.ClockId));
		}

		bool ParseBound(
			const TSharedPtr<FJsonObject>& Root,
			FTimelineAnimationBound& Out,
			FUeAgentTimelineAnimationExecutionError& Error)
		{
			Out = {};
			if (!RequiredString(Root, TEXT("kind"), Out.Kind, Error))
			{
				return false;
			}
			TSharedPtr<FJsonObject> Point;
			bool bPoint = false;
			if (!OptionalObject(Root, TEXT("point"), Point, bPoint, Error))
			{
				return false;
			}
			if (bPoint)
			{
				FTimelineAnimationPoint Parsed;
				if (!ParsePoint(Point, Parsed, Error))
				{
					return false;
				}
				Out.Point = MoveTemp(Parsed);
			}
			return Out.IsValid() || Fail(
				Error, TEXT("timeline_animation_bound_invalid"), Out.Kind);
		}

		bool ParseRange(
			const TSharedPtr<FJsonObject>& Root,
			FTimelineAnimationRange& Out,
			FUeAgentTimelineAnimationExecutionError& Error)
		{
			Out = {};
			TSharedPtr<FJsonObject> Lower;
			TSharedPtr<FJsonObject> Upper;
			return RequiredString(Root, TEXT("clock_id"), Out.ClockId, Error)
				&& RequiredString(Root, TEXT("state"), Out.State, Error)
				&& RequiredObject(Root, TEXT("lower"), Lower, Error)
				&& RequiredObject(Root, TEXT("upper"), Upper, Error)
				&& ParseBound(Lower, Out.Lower, Error)
				&& ParseBound(Upper, Out.Upper, Error)
				&& (Out.IsValid() || Fail(
					Error, TEXT("timeline_animation_range_invalid"), Out.ClockId));
		}

		bool ParseFrame(
			const TSharedPtr<FJsonObject>& Root,
			FTimelineAnimationFrame3d& Out,
			FUeAgentTimelineAnimationExecutionError& Error)
		{
			Out = {};
			return RequiredString(Root, TEXT("frame_id"), Out.FrameId, Error)
				&& RequiredString(Root, TEXT("basis_frame_id"), Out.BasisFrameId, Error)
				&& Root->TryGetNumberField(TEXT("dimension"), Out.Dimension)
				&& RequiredString(Root, TEXT("unit"), Out.Unit, Error)
				&& RequiredString(
					Root, TEXT("basis_evidence_id"), Out.BasisEvidenceId, Error)
				&& (Out.IsValid() || Fail(
					Error, TEXT("timeline_animation_frame_invalid"), Out.FrameId));
		}

		bool ParseTransform(
			const TSharedPtr<FJsonObject>& Root,
			FTimelineAnimationTransform3d& Out,
			FUeAgentTimelineAnimationExecutionError& Error)
		{
			Out = {};
			TSharedPtr<FJsonObject> Frame;
			return RequiredString(Root, TEXT("subject"), Out.Subject, Error)
				&& RequiredString(Root, TEXT("quantity"), Out.Quantity, Error)
				&& RequiredObject(Root, TEXT("frame"), Frame, Error)
				&& ParseFrame(Frame, Out.Frame, Error)
				&& ParseStringArray(Root, TEXT("components"), Out.Components, true, Error)
				&& ParseStringArray(
					Root, TEXT("conversion_evidence_ids"),
					Out.ConversionEvidenceIds, true, Error)
				&& (Out.IsValid() || Fail(
					Error, TEXT("timeline_animation_transform_invalid"), Out.Subject));
		}

		template <typename ItemType, typename ParseItemType>
		bool ParseObjectArray(
			const TSharedPtr<FJsonObject>& Root,
			const TCHAR* Field,
			TArray<ItemType>& Out,
			bool bRequired,
			ParseItemType ParseItem,
			FUeAgentTimelineAnimationExecutionError& Error)
		{
			Out.Reset();
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Root->TryGetArrayField(Field, Values) || !Values)
			{
				return !bRequired || Fail(
					Error, TEXT("timeline_animation_field_array_missing"), Field);
			}
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				const TSharedPtr<FJsonObject> Object =
					Value.IsValid() ? Value->AsObject() : nullptr;
				ItemType Item;
				if (!Object.IsValid() || !ParseItem(Object, Item, Error))
				{
					return false;
				}
				Out.Add(MoveTemp(Item));
			}
			return true;
		}

		bool ParseBoneKey(const TSharedPtr<FJsonObject>& Root,
			FTimelineAnimationBoneKey& Out,
			FUeAgentTimelineAnimationExecutionError& Error)
		{
			TSharedPtr<FJsonObject> Time;
			TSharedPtr<FJsonObject> Transform;
			return RequiredObject(Root, TEXT("time"), Time, Error)
				&& RequiredObject(Root, TEXT("transform"), Transform, Error)
				&& ParsePoint(Time, Out.Time, Error)
				&& ParseTransform(Transform, Out.Transform, Error)
				&& (Out.IsValid() || Fail(Error,
					TEXT("timeline_animation_bone_key_invalid")));
		}

		bool ParseBoneTrack(const TSharedPtr<FJsonObject>& Root,
			FTimelineAnimationBoneTrack& Out,
			FUeAgentTimelineAnimationExecutionError& Error)
		{
			return RequiredString(Root, TEXT("bone_ref"), Out.BoneRef, Error)
				&& ParseObjectArray(
					Root, TEXT("keys"), Out.Keys, true, ParseBoneKey, Error)
				&& (Out.IsValid() || Fail(Error,
					TEXT("timeline_animation_bone_track_invalid"), Out.BoneRef));
		}

		bool ParseCurveKey(const TSharedPtr<FJsonObject>& Root,
			FTimelineAnimationCurveKey& Out,
			FUeAgentTimelineAnimationExecutionError& Error)
		{
			TSharedPtr<FJsonObject> Time;
			return RequiredObject(Root, TEXT("time"), Time, Error)
				&& ParsePoint(Time, Out.Time, Error)
				&& Root->TryGetNumberField(TEXT("value"), Out.Value)
				&& RequiredString(Root, TEXT("interpolation"), Out.Interpolation, Error)
				&& (Out.IsValid() || Fail(Error,
					TEXT("timeline_animation_curve_key_invalid")));
		}

		bool ParseCurveChange(const TSharedPtr<FJsonObject>& Root,
			FTimelineAnimationCurveChange& Out,
			FUeAgentTimelineAnimationExecutionError& Error)
		{
			return RequiredString(Root, TEXT("curve_ref"), Out.CurveRef, Error)
				&& RequiredString(Root, TEXT("operation"), Out.Operation, Error)
				&& ParseObjectArray(
					Root, TEXT("keys"), Out.Keys, true, ParseCurveKey, Error)
				&& (Out.IsValid() || Fail(Error,
					TEXT("timeline_animation_curve_change_invalid"), Out.CurveRef));
		}

		bool ParseMetadataChange(const TSharedPtr<FJsonObject>& Root,
			FTimelineAnimationMetadataChange& Out,
			FUeAgentTimelineAnimationExecutionError& Error)
		{
			return RequiredString(Root, TEXT("metadata_ref"), Out.MetadataRef, Error)
				&& RequiredString(Root, TEXT("operation"), Out.Operation, Error)
				&& RequiredString(Root, TEXT("value_type"), Out.ValueType, Error)
				&& RequiredString(Root, TEXT("value_lexeme"), Out.ValueLexeme, Error)
				&& (Out.IsValid() || Fail(Error,
					TEXT("timeline_animation_metadata_change_invalid"), Out.MetadataRef));
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
				if (!ParsePoint(Object, Point, Error)) return false;
				Out.Time = MoveTemp(Point);
			}
			if (!OptionalObject(Root, TEXT("range"), Object, bPresent, Error))
			{
				return false;
			}
			if (bPresent)
			{
				FTimelineAnimationRange Range;
				if (!ParseRange(Object, Range, Error)) return false;
				Out.Range = MoveTemp(Range);
			}
			return Out.IsValid() || Fail(Error,
				TEXT("timeline_animation_event_change_invalid"), Out.EventRef);
		}

		bool ParseBlendParameter(const TSharedPtr<FJsonObject>& Root,
			FTimelineAnimationBlendParameter& Out,
			FUeAgentTimelineAnimationExecutionError& Error)
		{
			return RequiredString(Root, TEXT("axis_ref"), Out.AxisRef, Error)
				&& Root->TryGetNumberField(TEXT("value"), Out.Value)
				&& (Out.IsValid() || Fail(Error,
					TEXT("timeline_animation_blend_parameter_invalid"), Out.AxisRef));
		}

		bool ParseTimelineKey(const TSharedPtr<FJsonObject>& Root,
			FTimelineAnimationKey& Out,
			FUeAgentTimelineAnimationExecutionError& Error)
		{
			TSharedPtr<FJsonObject> Time;
			if (!RequiredString(Root, TEXT("key_ref"), Out.KeyRef, Error)
				|| !RequiredObject(Root, TEXT("time"), Time, Error)
				|| !ParsePoint(Time, Out.Time, Error)
				|| !RequiredString(Root, TEXT("value_type"), Out.ValueType, Error)
				|| !OptionalString(Root, TEXT("value_lexeme"), Out.ValueLexeme, Error))
			{
				return false;
			}
			TSharedPtr<FJsonObject> Transform;
			bool bPresent = false;
			if (!OptionalObject(Root, TEXT("transform"), Transform, bPresent, Error))
			{
				return false;
			}
			if (bPresent)
			{
				FTimelineAnimationTransform3d Parsed;
				if (!ParseTransform(Transform, Parsed, Error)) return false;
				Out.Transform = MoveTemp(Parsed);
			}
			return Out.IsValid() || Fail(Error,
				TEXT("timeline_animation_timeline_key_invalid"), Out.KeyRef);
		}

		bool ParseTimelineSection(const TSharedPtr<FJsonObject>& Root,
			FTimelineAnimationSection& Out,
			FUeAgentTimelineAnimationExecutionError& Error)
		{
			TSharedPtr<FJsonObject> Range;
			return RequiredString(Root, TEXT("section_ref"), Out.SectionRef, Error)
				&& RequiredString(Root, TEXT("section_kind"), Out.SectionKind, Error)
				&& RequiredObject(Root, TEXT("range"), Range, Error)
				&& ParseRange(Range, Out.Range, Error)
				&& OptionalString(Root, TEXT("source_ref"), Out.SourceRef, Error)
				&& ParseObjectArray(
					Root, TEXT("keys"), Out.Keys, true, ParseTimelineKey, Error)
				&& (Out.IsValid() || Fail(Error,
					TEXT("timeline_animation_section_invalid"), Out.SectionRef));
		}

		bool ParseTimelineTrack(const TSharedPtr<FJsonObject>& Root,
			FTimelineAnimationTrack& Out,
			FUeAgentTimelineAnimationExecutionError& Error)
		{
			return RequiredString(Root, TEXT("track_ref"), Out.TrackRef, Error)
				&& RequiredString(Root, TEXT("track_kind"), Out.TrackKind, Error)
				&& OptionalString(Root, TEXT("property_path"), Out.PropertyPath, Error)
				&& ParseObjectArray(
					Root, TEXT("sections"), Out.Sections, true, ParseTimelineSection, Error)
				&& (Out.IsValid() || Fail(Error,
					TEXT("timeline_animation_track_invalid"), Out.TrackRef));
		}

		bool ParseTimelineBinding(const TSharedPtr<FJsonObject>& Root,
			FTimelineAnimationBinding& Out,
			FUeAgentTimelineAnimationExecutionError& Error)
		{
			return RequiredString(Root, TEXT("binding_ref"), Out.BindingRef, Error)
				&& RequiredString(Root, TEXT("binding_kind"), Out.BindingKind, Error)
				&& OptionalString(Root, TEXT("object_ref"), Out.ObjectRef, Error)
				&& ParseObjectArray(
					Root, TEXT("tracks"), Out.Tracks, true, ParseTimelineTrack, Error)
				&& (Out.IsValid() || Fail(Error,
					TEXT("timeline_animation_binding_invalid"), Out.BindingRef));
		}

		bool ParseStructure(const TSharedPtr<FJsonObject>& Root,
			FTimelineAnimationStructure& Out,
			FUeAgentTimelineAnimationExecutionError& Error)
		{
			TSharedPtr<FJsonObject> Display;
			TSharedPtr<FJsonObject> Tick;
			TSharedPtr<FJsonObject> Range;
			return RequiredObject(Root, TEXT("display_rate"), Display, Error)
				&& ParseRate(Display, Out.DisplayRate, Error)
				&& RequiredObject(Root, TEXT("tick_resolution"), Tick, Error)
				&& ParseRate(Tick, Out.TickResolution, Error)
				&& RequiredObject(Root, TEXT("playback_range"), Range, Error)
				&& ParseRange(Range, Out.PlaybackRange, Error)
				&& ParseObjectArray(
					Root, TEXT("bindings"), Out.Bindings, true, ParseTimelineBinding, Error)
				&& ParseObjectArray(
					Root, TEXT("root_tracks"), Out.RootTracks, true, ParseTimelineTrack, Error)
				&& (Out.IsValid() || Fail(Error,
					TEXT("timeline_animation_structure_invalid")));
		}

		bool ParsePoseItem(const TSharedPtr<FJsonObject>& Root,
			FTimelineAnimationPoseItem& Out,
			FUeAgentTimelineAnimationExecutionError& Error)
		{
			TSharedPtr<FJsonObject> Transform;
			return RequiredString(Root, TEXT("bone_ref"), Out.BoneRef, Error)
				&& RequiredObject(Root, TEXT("transform"), Transform, Error)
				&& ParseTransform(Transform, Out.Transform, Error)
				&& (Out.IsValid() || Fail(Error,
					TEXT("timeline_animation_pose_item_invalid"), Out.BoneRef));
		}

		bool ParseMorphWeight(const TSharedPtr<FJsonObject>& Root,
			FTimelineAnimationMorphWeight& Out,
			FUeAgentTimelineAnimationExecutionError& Error)
		{
			return RequiredString(Root, TEXT("morph_ref"), Out.MorphRef, Error)
				&& Root->TryGetNumberField(TEXT("weight"), Out.Weight)
				&& (Out.IsValid() || Fail(Error,
					TEXT("timeline_animation_morph_weight_invalid"), Out.MorphRef));
		}

		bool ConvertCanonicalValue(
			const TSharedPtr<FJsonValue>& JsonValue,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentTimelineAnimationExecutionError& Error)
		{
			OutValue.Reset();
			if (!JsonValue.IsValid())
			{
				return Fail(Error, TEXT("timeline_animation_intent_json_invalid"));
			}
			FUeAgentStateSemanticError StateError;
			switch (JsonValue->Type)
			{
			case EJson::Null:
				return FUeAgentCanonicalValue::TryNull(Resources, OutValue, StateError)
					|| Fail(Error, StateError.Code);
			case EJson::Boolean:
				return FUeAgentCanonicalValue::TryBoolean(
					JsonValue->AsBool(), Resources, OutValue, StateError)
					|| Fail(Error, StateError.Code);
			case EJson::String:
				return FUeAgentCanonicalValue::TryString(
					JsonValue->AsString(), Resources, OutValue, StateError)
					|| Fail(Error, StateError.Code);
			case EJson::Number:
			{
				FString Lexeme;
				if (!JsonValue->TryGetString(Lexeme))
				{
					Lexeme = FString::SanitizeFloat(JsonValue->AsNumber(), 0);
				}
				int64 Integer = 0;
				const bool bInteger = LexTryParseString(Integer, *Lexeme)
					&& !Lexeme.Contains(TEXT("."))
					&& !Lexeme.Contains(TEXT("e"), ESearchCase::IgnoreCase);
				return (bInteger
					? FUeAgentCanonicalValue::TryInteger(
						Lexeme, Resources, OutValue, StateError)
					: FUeAgentCanonicalValue::TryDecimal(
						Lexeme, Resources, OutValue, StateError))
					|| Fail(Error, StateError.Code);
			}
			case EJson::Array:
			{
				TArray<FUeAgentCanonicalValueRef> Items;
				for (const TSharedPtr<FJsonValue>& Item : JsonValue->AsArray())
				{
					FUeAgentCanonicalValuePtr Canonical;
					if (!ConvertCanonicalValue(Item, Resources, Canonical, Error))
					{
						return false;
					}
					Items.Add(Canonical.ToSharedRef());
				}
				return FUeAgentCanonicalValue::TryArray(
					Items, Resources, OutValue, StateError)
					|| Fail(Error, StateError.Code);
			}
			case EJson::Object:
			{
				TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items;
				TArray<FString> Keys;
				JsonValue->AsObject()->Values.GetKeys(Keys);
				Keys.Sort();
				for (const FString& Key : Keys)
				{
					FUeAgentCanonicalValuePtr Canonical;
					if (!ConvertCanonicalValue(
							JsonValue->AsObject()->Values.FindChecked(Key),
							Resources, Canonical, Error))
					{
						return false;
					}
					Items.Emplace(Key, Canonical.ToSharedRef());
				}
				return FUeAgentCanonicalValue::TryObject(
					Items, Resources, OutValue, StateError)
					|| Fail(Error, StateError.Code);
			}
			default:
				return Fail(Error, TEXT("timeline_animation_intent_json_kind_invalid"));
			}
		}

		bool ParseOptionalRange(
			const TSharedPtr<FJsonObject>& Root,
			const TCHAR* Field,
			TOptional<FTimelineAnimationRange>& Out,
			FUeAgentTimelineAnimationExecutionError& Error)
		{
			Out.Reset();
			TSharedPtr<FJsonObject> Object;
			bool bPresent = false;
			if (!OptionalObject(Root, Field, Object, bPresent, Error)) return false;
			if (!bPresent) return true;
			FTimelineAnimationRange Value;
			if (!ParseRange(Object, Value, Error)) return false;
			Out = MoveTemp(Value);
			return true;
		}

		bool ParseOptionalPoint(
			const TSharedPtr<FJsonObject>& Root,
			const TCHAR* Field,
			TOptional<FTimelineAnimationPoint>& Out,
			FUeAgentTimelineAnimationExecutionError& Error)
		{
			Out.Reset();
			TSharedPtr<FJsonObject> Object;
			bool bPresent = false;
			if (!OptionalObject(Root, Field, Object, bPresent, Error)) return false;
			if (!bPresent) return true;
			FTimelineAnimationPoint Value;
			if (!ParsePoint(Object, Value, Error)) return false;
			Out = MoveTemp(Value);
			return true;
		}

		bool ParseOptionalRate(
			const TSharedPtr<FJsonObject>& Root,
			const TCHAR* Field,
			TOptional<FTimelineAnimationRate>& Out,
			FUeAgentTimelineAnimationExecutionError& Error)
		{
			Out.Reset();
			TSharedPtr<FJsonObject> Object;
			bool bPresent = false;
			if (!OptionalObject(Root, Field, Object, bPresent, Error)) return false;
			if (!bPresent) return true;
			FTimelineAnimationRate Value;
			if (!ParseRate(Object, Value, Error)) return false;
			Out = MoveTemp(Value);
			return true;
		}

		bool ParseRequiredRevision(
			const TSharedPtr<FJsonObject>& Root,
			FTimelineAnimationRequestData& Data,
			FUeAgentTimelineAnimationExecutionError& Error)
		{
			TSharedPtr<FJsonObject> Object;
			FTimelineAnimationRevision Value;
			if (!RequiredObject(Root, TEXT("expected_revision"), Object, Error)
				|| !ParseRevision(Object, Value, Error))
			{
				return false;
			}
			Data.ExpectedRevision = MoveTemp(Value);
			return true;
		}

		bool DecodeData(
			EUeAgentTimelineAnimationRequestKind Kind,
			const TSharedPtr<FJsonObject>& Root,
			FTimelineAnimationRequestData& Data,
			FUeAgentTimelineAnimationExecutionError& Error)
		{
			TSharedPtr<FJsonObject> Object;
			bool bPresent = false;
			switch (Kind)
			{
			case EUeAgentTimelineAnimationRequestKind::SequenceRead:
				return ParseOptionalRange(Root, TEXT("temporal_range"), Data.TemporalRange, Error)
					&& OptionalBool(Root, TEXT("include_tracks"), Data.IncludeTracks, Error)
					&& OptionalBool(Root, TEXT("include_keys"), Data.IncludeKeys, Error);
			case EUeAgentTimelineAnimationRequestKind::SequenceRootMotionAnalyze:
				if (!RequiredObject(Root, TEXT("temporal_range"), Object, Error)) return false;
				{
					FTimelineAnimationRange Value;
					if (!ParseRange(Object, Value, Error)) return false;
					Data.TemporalRange = MoveTemp(Value);
				}
				if (!RequiredObject(Root, TEXT("sample_rate"), Object, Error)) return false;
				{
					FTimelineAnimationRate Value;
					if (!ParseRate(Object, Value, Error)) return false;
					Data.SampleRate = MoveTemp(Value);
				}
				return OptionalString(Root, TEXT("root_bone_ref"), Data.RootBoneRef, Error);
			case EUeAgentTimelineAnimationRequestKind::SequencePoseSample:
				if (!RequiredObject(Root, TEXT("time_point"), Object, Error)) return false;
				{
					FTimelineAnimationPoint Value;
					if (!ParsePoint(Object, Value, Error)) return false;
					Data.TimePoint = MoveTemp(Value);
				}
				if (!ParseStringArray(Root, TEXT("bone_refs"), Data.BoneRefs, false, Error)) return false;
				if (!OptionalObject(Root, TEXT("output_frame"), Object, bPresent, Error)) return false;
				if (bPresent)
				{
					FTimelineAnimationFrame3d Frame;
					if (!ParseFrame(Object, Frame, Error)) return false;
					Data.OutputFrame = MoveTemp(Frame);
				}
				return true;
			case EUeAgentTimelineAnimationRequestKind::BlendSpaceSample:
				if (!ParseObjectArray(Root, TEXT("blend_parameters"), Data.BlendParameters, true, ParseBlendParameter, Error)
					|| !ParseOptionalPoint(Root, TEXT("time_point"), Data.TimePoint, Error)
					|| !ParseStringArray(Root, TEXT("bone_refs"), Data.BoneRefs, false, Error)
					|| !OptionalObject(Root, TEXT("output_frame"), Object, bPresent, Error)) return false;
				if (bPresent)
				{
					FTimelineAnimationFrame3d Frame;
					if (!ParseFrame(Object, Frame, Error)) return false;
					Data.OutputFrame = MoveTemp(Frame);
				}
				return true;
			case EUeAgentTimelineAnimationRequestKind::SequenceBoneTracksReplace:
				if (!RequiredObject(Root, TEXT("authored_range"), Object, Error)) return false;
				{
					FTimelineAnimationRange Value;
					if (!ParseRange(Object, Value, Error)) return false;
					Data.TemporalRange = MoveTemp(Value);
				}
				if (!RequiredObject(Root, TEXT("sample_rate"), Object, Error)) return false;
				{
					FTimelineAnimationRate Value;
					if (!ParseRate(Object, Value, Error)) return false;
					Data.SampleRate = MoveTemp(Value);
				}
				return ParseObjectArray(Root, TEXT("bone_tracks"), Data.BoneTracks, true, ParseBoneTrack, Error)
					&& ParseRequiredRevision(Root, Data, Error);
			case EUeAgentTimelineAnimationRequestKind::SequenceCurvesUpdate:
				return ParseObjectArray(Root, TEXT("curve_changes"), Data.CurveChanges, true, ParseCurveChange, Error)
					&& ParseRequiredRevision(Root, Data, Error);
			case EUeAgentTimelineAnimationRequestKind::SequenceMetadataUpdate:
				return ParseObjectArray(Root, TEXT("metadata_changes"), Data.MetadataChanges, true, ParseMetadataChange, Error)
					&& ParseRequiredRevision(Root, Data, Error);
			case EUeAgentTimelineAnimationRequestKind::SequenceEventsUpdate:
				return ParseObjectArray(Root, TEXT("event_changes"), Data.EventChanges, true, ParseEventChange, Error)
					&& ParseRequiredRevision(Root, Data, Error);
			case EUeAgentTimelineAnimationRequestKind::MontageRead:
				return OptionalBool(Root, TEXT("include_sections"), Data.IncludeSections, Error)
					&& OptionalBool(Root, TEXT("include_slots"), Data.IncludeSlots, Error);
			case EUeAgentTimelineAnimationRequestKind::MontageCreate:
				if (!RequiredString(Root, TEXT("destination_ref"), Data.DestinationRef, Error)
					|| !RequiredString(Root, TEXT("skeleton_ref"), Data.SkeletonRef, Error)
					|| !RequiredObject(Root, TEXT("initial_structure"), Object, Error)) return false;
				{
					FTimelineAnimationStructure Structure;
					if (!ParseStructure(Object, Structure, Error)) return false;
					Data.Structure = MoveTemp(Structure);
				}
				return OptionalString(Root, TEXT("source_animation_ref"), Data.SourceAnimationRef, Error);
			case EUeAgentTimelineAnimationRequestKind::MontageStructureReplace:
				if (!RequiredObject(Root, TEXT("structure"), Object, Error)) return false;
				{
					FTimelineAnimationStructure Structure;
					if (!ParseStructure(Object, Structure, Error)) return false;
					Data.Structure = MoveTemp(Structure);
				}
				return ParseRequiredRevision(Root, Data, Error);
			case EUeAgentTimelineAnimationRequestKind::SkeletonSlotsRead:
				return true;
			case EUeAgentTimelineAnimationRequestKind::LevelSequenceRead:
				return ParseOptionalRange(Root, TEXT("temporal_range"), Data.TemporalRange, Error)
					&& OptionalBool(Root, TEXT("include_bindings"), Data.IncludeBindings, Error)
					&& OptionalBool(Root, TEXT("include_tracks"), Data.IncludeTracks, Error)
					&& OptionalBool(Root, TEXT("include_keys"), Data.IncludeKeys, Error);
			case EUeAgentTimelineAnimationRequestKind::LevelSequenceCreate:
				if (!RequiredString(Root, TEXT("destination_ref"), Data.DestinationRef, Error)
					|| !RequiredObject(Root, TEXT("initial_structure"), Object, Error)) return false;
				{
					FTimelineAnimationStructure Structure;
					if (!ParseStructure(Object, Structure, Error)) return false;
					Data.Structure = MoveTemp(Structure);
				}
				return ParseOptionalRate(Root, TEXT("display_rate"), Data.DisplayRate, Error)
					&& ParseOptionalRate(Root, TEXT("tick_resolution"), Data.TickResolution, Error);
			case EUeAgentTimelineAnimationRequestKind::LevelSequenceSettingsUpdate:
				return ParseOptionalRate(
						Root, TEXT("display_rate"), Data.DisplayRate, Error)
					&& ParseOptionalRange(
						Root, TEXT("playback_range"), Data.TemporalRange, Error)
					&& ParseRequiredRevision(Root, Data, Error);
			case EUeAgentTimelineAnimationRequestKind::LevelSequenceStructureReplace:
				if (!RequiredObject(Root, TEXT("structure"), Object, Error)) return false;
				{
					FTimelineAnimationStructure Structure;
					if (!ParseStructure(Object, Structure, Error)) return false;
					Data.Structure = MoveTemp(Structure);
				}
				return ParseRequiredRevision(Root, Data, Error);
			case EUeAgentTimelineAnimationRequestKind::WidgetAnimationRead:
				return ParseOptionalRange(Root, TEXT("temporal_range"), Data.TemporalRange, Error)
					&& OptionalBool(Root, TEXT("include_bindings"), Data.IncludeBindings, Error)
					&& OptionalBool(Root, TEXT("include_tracks"), Data.IncludeTracks, Error);
			case EUeAgentTimelineAnimationRequestKind::SubjectPoseUpdate:
				if (!ParseOptionalPoint(Root, TEXT("evaluation_point"), Data.EvaluationPoint, Error)
					|| !Data.EvaluationPoint.IsSet()
					|| !ParseObjectArray(Root, TEXT("pose"), Data.Pose, true, ParsePoseItem, Error)
					|| !RequiredObject(Root, TEXT("pose_frame"), Object, Error)) return false;
				{
					FTimelineAnimationFrame3d Frame;
					if (!ParseFrame(Object, Frame, Error)) return false;
					Data.PoseFrame = MoveTemp(Frame);
				}
				return OptionalNumber(Root, TEXT("weight"), Data.Weight, Error);
			case EUeAgentTimelineAnimationRequestKind::SubjectMorphWeightsUpdate:
				return ParseOptionalPoint(Root, TEXT("evaluation_point"), Data.EvaluationPoint, Error)
					&& Data.EvaluationPoint.IsSet()
					&& ParseObjectArray(Root, TEXT("morph_weights"), Data.MorphWeights, true, ParseMorphWeight, Error);
			case EUeAgentTimelineAnimationRequestKind::IkProbeCandidateApply:
				if (!RequiredString(Root, TEXT("rig_ref"), Data.RigRef, Error)
					|| !RequiredString(Root, TEXT("goal_ref"), Data.GoalRef, Error)
					|| !RequiredObject(Root, TEXT("candidate"), Object, Error)) return false;
				{
					FTimelineAnimationTransform3d Transform;
					if (!ParseTransform(Object, Transform, Error)) return false;
					Data.Candidate = MoveTemp(Transform);
				}
				if (!RequiredObject(Root, TEXT("authored_time"), Object, Error)) return false;
				{
					FTimelineAnimationPoint Point;
					if (!ParsePoint(Object, Point, Error)) return false;
					Data.AuthoredTime = MoveTemp(Point);
				}
				return ParseRequiredRevision(Root, Data, Error);
			default:
				return Fail(Error, TEXT("timeline_animation_request_kind_invalid"));
			}
		}

		struct FRequestSpec
		{
			const TCHAR* CapabilityId;
			const TCHAR* TypeId;
			const TCHAR* FamilyId;
			const TCHAR* RequestKindId;
			EUeAgentTimelineAnimationRequestKind Kind;
			bool bOperation;
		};

		const TArray<FRequestSpec>& RequestSpecs()
		{
			static const TArray<FRequestSpec> Specs = {
				{TEXT("animation.sequence.read"), TEXT("timeline_animation.sequence.read.request.v1"), TEXT("temporal_read"), TEXT("sequence_read"), EUeAgentTimelineAnimationRequestKind::SequenceRead, false},
				{TEXT("animation.sequence.root_motion.analyze"), TEXT("timeline_animation.sequence.root_motion.request.v1"), TEXT("temporal_read"), TEXT("sequence_root_motion_analyze"), EUeAgentTimelineAnimationRequestKind::SequenceRootMotionAnalyze, false},
				{TEXT("animation.sequence.pose.sample"), TEXT("timeline_animation.sequence.pose_sample.request.v1"), TEXT("temporal_read"), TEXT("sequence_pose_sample"), EUeAgentTimelineAnimationRequestKind::SequencePoseSample, false},
				{TEXT("animation.blend_space.sample"), TEXT("timeline_animation.blend_space.sample.request.v1"), TEXT("temporal_read"), TEXT("blend_space_sample"), EUeAgentTimelineAnimationRequestKind::BlendSpaceSample, false},
				{TEXT("animation.sequence.bone_tracks.replace"), TEXT("timeline_animation.sequence.bone_tracks_replace.request.v1"), TEXT("animation_keyed_data"), TEXT("sequence_bone_tracks_replace"), EUeAgentTimelineAnimationRequestKind::SequenceBoneTracksReplace, true},
				{TEXT("animation.sequence.curves.update"), TEXT("timeline_animation.sequence.curves_update.request.v1"), TEXT("animation_keyed_data"), TEXT("sequence_curves_update"), EUeAgentTimelineAnimationRequestKind::SequenceCurvesUpdate, true},
				{TEXT("animation.sequence.metadata.update"), TEXT("timeline_animation.sequence.metadata_update.request.v1"), TEXT("animation_keyed_data"), TEXT("sequence_metadata_update"), EUeAgentTimelineAnimationRequestKind::SequenceMetadataUpdate, true},
				{TEXT("animation.sequence.events.update"), TEXT("timeline_animation.sequence.events_update.request.v1"), TEXT("animation_keyed_data"), TEXT("sequence_events_update"), EUeAgentTimelineAnimationRequestKind::SequenceEventsUpdate, true},
				{TEXT("animation.montage.read"), TEXT("timeline_animation.montage.read.request.v1"), TEXT("temporal_read"), TEXT("montage_read"), EUeAgentTimelineAnimationRequestKind::MontageRead, false},
				{TEXT("animation.montage.create"), TEXT("timeline_animation.montage.create.request.v1"), TEXT("timeline_structure"), TEXT("montage_create"), EUeAgentTimelineAnimationRequestKind::MontageCreate, true},
				{TEXT("animation.montage.structure.replace"), TEXT("timeline_animation.montage.structure_replace.request.v1"), TEXT("timeline_structure"), TEXT("montage_structure_replace"), EUeAgentTimelineAnimationRequestKind::MontageStructureReplace, true},
				{TEXT("animation.skeleton.slots.read"), TEXT("timeline_animation.skeleton.slots_read.request.v1"), TEXT("temporal_read"), TEXT("skeleton_slots_read"), EUeAgentTimelineAnimationRequestKind::SkeletonSlotsRead, false},
				{TEXT("timeline.sequence.read"), TEXT("timeline_animation.level_sequence.read.request.v1"), TEXT("temporal_read"), TEXT("level_sequence_read"), EUeAgentTimelineAnimationRequestKind::LevelSequenceRead, false},
				{TEXT("timeline.sequence.create"), TEXT("timeline_animation.level_sequence.create.request.v1"), TEXT("timeline_structure"), TEXT("level_sequence_create"), EUeAgentTimelineAnimationRequestKind::LevelSequenceCreate, true},
				{TEXT("timeline.sequence.settings.update"), TEXT("timeline_animation.level_sequence.settings_update.request.v1"), TEXT("timeline_structure"), TEXT("level_sequence_settings_update"), EUeAgentTimelineAnimationRequestKind::LevelSequenceSettingsUpdate, true},
				{TEXT("timeline.sequence.structure.replace"), TEXT("timeline_animation.level_sequence.structure_replace.request.v1"), TEXT("timeline_structure"), TEXT("level_sequence_structure_replace"), EUeAgentTimelineAnimationRequestKind::LevelSequenceStructureReplace, true},
				{TEXT("timeline.widget_animation.read"), TEXT("timeline_animation.widget_animation.read.request.v1"), TEXT("temporal_read"), TEXT("widget_animation_read"), EUeAgentTimelineAnimationRequestKind::WidgetAnimationRead, false},
				{TEXT("animation.subject.pose.update"), TEXT("timeline_animation.subject.pose_update.request.v1"), TEXT("subject_animation_state"), TEXT("subject_pose_update"), EUeAgentTimelineAnimationRequestKind::SubjectPoseUpdate, true},
				{TEXT("animation.subject.morph_weights.update"), TEXT("timeline_animation.subject.morph_weights_update.request.v1"), TEXT("subject_animation_state"), TEXT("subject_morph_weights_update"), EUeAgentTimelineAnimationRequestKind::SubjectMorphWeightsUpdate, true},
				{TEXT("animation.ik_probe_candidate.apply"), TEXT("timeline_animation.ik_probe.candidate_apply.request.v1"), TEXT("timeline_rig_candidate"), TEXT("ik_probe_candidate_apply"), EUeAgentTimelineAnimationRequestKind::IkProbeCandidateApply, true},
			};
			return Specs;
		}

		class FTimelineAnimationRequestDecoder final
			: public IUeAgentTimelineAnimationRequestDecoder
		{
		public:
			bool TryDecode(
				const FUeAgentTimelineAnimationExecutionChainContract& Contract,
				const FUeAgentContractValue& CanonicalInput,
				FUeAgentResourceTracker& Resources,
				FUeAgentTimelineAnimationTypedRequestPtr& OutRequest,
				FUeAgentTimelineAnimationExecutionError& OutError) override
			{
				OutRequest.Reset();
				OutError = {};
				const FRequestSpec* Spec = RequestSpecs().FindByPredicate(
					[&Contract](const FRequestSpec& Candidate)
					{
						return Contract.GetCapabilityId() == Candidate.CapabilityId;
					});
				if (!Spec || Contract.GetTypedRequestId() != Spec->TypeId
					|| Contract.GetFamilyId() != Spec->FamilyId
					|| Contract.RequiresNativeMutation() != Spec->bOperation)
				{
					return Fail(
						OutError, TEXT("timeline_animation_request_contract_mismatch"),
						Contract.GetCapabilityId());
				}
				TSharedPtr<FJsonObject> Root;
				FString JsonError;
				if (!Json::TryParseObject(CanonicalInput.GetJsonValue(), Root, JsonError)
					|| !Root.IsValid())
				{
					return Fail(
						OutError, TEXT("timeline_animation_request_json_invalid"),
						Contract.GetCapabilityId(), JsonError);
				}
				FTimelineAnimationRequestData Data;
				if (!DecodeData(Spec->Kind, Root, Data, OutError)
					|| !Data.IsValidFor(Spec->Kind))
				{
					if (!OutError.IsValid())
					{
						Fail(OutError, TEXT("timeline_animation_request_data_invalid"),
							Contract.GetCapabilityId());
					}
					return false;
				}
				FUeAgentCanonicalValuePtr Intent;
				if (!ConvertCanonicalValue(
						MakeShared<FJsonValueObject>(Root), Resources, Intent, OutError)
					|| !Intent.IsValid())
				{
					return false;
				}
				OutRequest = MakeShared<FUeAgentTimelineAnimationRequest>(
					Spec->TypeId, Spec->FamilyId, Spec->RequestKindId,
					Spec->Kind, Spec->bOperation, MoveTemp(Data), Intent.ToSharedRef());
				return OutRequest.IsValid();
			}
		};
	}

	bool FTimelineAnimationRevision::IsValid() const
	{
		return !SourceId.IsEmpty() && !Generation.IsEmpty() && Sequence >= 0;
	}

	bool FTimelineAnimationRate::IsValid() const
	{
		return FUeAgentTemporalRate(Numerator, Denominator).IsValid();
	}

	bool FTimelineAnimationPoint::IsValid() const
	{
		return State == TEXT("authored") && Rate.IsValid()
			&& ToTemporalPoint().IsValid();
	}

	FUeAgentTemporalPoint FTimelineAnimationPoint::ToTemporalPoint() const
	{
		return FUeAgentTemporalPoint(
			ClockId, Tick, Rate.Numerator, Rate.Denominator, State);
	}

	bool FTimelineAnimationBound::IsValid() const
	{
		if (Kind == TEXT("open")) return !Point.IsSet();
		return (Kind == TEXT("inclusive") || Kind == TEXT("exclusive"))
			&& Point.IsSet() && Point->IsValid();
	}

	bool FTimelineAnimationRange::IsValid() const
	{
		if (ClockId.IsEmpty() || State != TEXT("authored")
			|| !Lower.IsValid() || !Upper.IsValid()) return false;
		auto ToBound = [](const FTimelineAnimationBound& Bound)
		{
			if (Bound.Kind == TEXT("open")) return FUeAgentTemporalBound::Open();
			return Bound.Kind == TEXT("inclusive")
				? FUeAgentTemporalBound::Inclusive(Bound.Point->ToTemporalPoint())
				: FUeAgentTemporalBound::Exclusive(Bound.Point->ToTemporalPoint());
		};
		return FUeAgentTemporalRange(
			ClockId, State, ToBound(Lower), ToBound(Upper)).IsValid();
	}

	bool FTimelineAnimationFrame3d::IsValid() const
	{
		return !FrameId.IsEmpty() && !BasisFrameId.IsEmpty()
			&& Dimension == 3 && !Unit.IsEmpty() && !BasisEvidenceId.IsEmpty();
	}

	bool FTimelineAnimationTransform3d::IsValid() const
	{
		return !Subject.IsEmpty() && Quantity == TEXT("transform")
			&& Frame.IsValid() && Components.Num() == 10
			&& !ConversionEvidenceIds.IsEmpty();
	}

	bool FTimelineAnimationTransform3d::TryToNativeTransform(
		const FString& NativeFrameId,
		FTransform& OutTransform,
		FUeAgentTimelineAnimationExecutionError& OutError) const
	{
		OutTransform = FTransform::Identity;
		if (!IsValid() || Frame.FrameId != NativeFrameId
			|| Frame.Unit != TEXT("centimeter"))
		{
			return Fail(OutError,
				TEXT("timeline_animation_space_conversion_unavailable"),
				Subject, Frame.FrameId + TEXT(" -> ") + NativeFrameId);
		}
		TArray<double> Values;
		for (const FString& Component : Components)
		{
			double Value = 0.0;
			if (!LexTryParseString(Value, *Component) || !FMath::IsFinite(Value))
			{
				return Fail(OutError,
					TEXT("timeline_animation_transform_component_invalid"),
					Subject, Component);
			}
			Values.Add(Value);
		}
		FQuat Rotation(Values[3], Values[4], Values[5], Values[6]);
		if (!Rotation.IsNormalized()) Rotation.Normalize();
		OutTransform = FTransform(
			Rotation,
			FVector(Values[0], Values[1], Values[2]),
			FVector(Values[7], Values[8], Values[9]));
		return !OutTransform.ContainsNaN();
	}

	bool FTimelineAnimationBoneKey::IsValid() const { return Time.IsValid() && Transform.IsValid(); }
	bool FTimelineAnimationBoneTrack::IsValid() const { return !BoneRef.IsEmpty() && !Keys.IsEmpty() && !Keys.ContainsByPredicate([](const auto& V){ return !V.IsValid(); }); }
	bool FTimelineAnimationCurveKey::IsValid() const { return Time.IsValid() && FMath::IsFinite(Value) && (Interpolation == TEXT("constant") || Interpolation == TEXT("linear") || Interpolation == TEXT("cubic")); }
	bool FTimelineAnimationCurveChange::IsValid() const { return !CurveRef.IsEmpty() && (Operation == TEXT("upsert") || Operation == TEXT("remove")) && !Keys.ContainsByPredicate([](const auto& V){ return !V.IsValid(); }); }
	bool FTimelineAnimationMetadataChange::IsValid() const { return !MetadataRef.IsEmpty() && (Operation == TEXT("upsert") || Operation == TEXT("remove")) && (ValueType == TEXT("string") || ValueType == TEXT("integer") || ValueType == TEXT("decimal") || ValueType == TEXT("boolean") || ValueType == TEXT("asset_ref")); }
	bool FTimelineAnimationEventChange::IsValid() const { return !EventRef.IsEmpty() && (Operation == TEXT("upsert") || Operation == TEXT("remove")) && (EventKind == TEXT("notify_track") || EventKind == TEXT("notify") || EventKind == TEXT("notify_state") || EventKind == TEXT("sync_marker")) && (!Time.IsSet() || Time->IsValid()) && (!Range.IsSet() || Range->IsValid()); }
	bool FTimelineAnimationBlendParameter::IsValid() const { return !AxisRef.IsEmpty() && FMath::IsFinite(Value); }
	bool FTimelineAnimationKey::IsValid() const { return !KeyRef.IsEmpty() && Time.IsValid() && !ValueType.IsEmpty() && (ValueType != TEXT("transform") || (Transform.IsSet() && Transform->IsValid())); }
	bool FTimelineAnimationSection::IsValid() const { return !SectionRef.IsEmpty() && !SectionKind.IsEmpty() && Range.IsValid() && !Keys.ContainsByPredicate([](const auto& V){ return !V.IsValid(); }); }
	bool FTimelineAnimationTrack::IsValid() const { return !TrackRef.IsEmpty() && !TrackKind.IsEmpty() && !Sections.ContainsByPredicate([](const auto& V){ return !V.IsValid(); }); }
	bool FTimelineAnimationBinding::IsValid() const { return !BindingRef.IsEmpty() && (BindingKind == TEXT("possessable") || BindingKind == TEXT("spawnable") || BindingKind == TEXT("widget") || BindingKind == TEXT("slot")) && !Tracks.ContainsByPredicate([](const auto& V){ return !V.IsValid(); }); }
	bool FTimelineAnimationStructure::IsValid() const { return DisplayRate.IsValid() && TickResolution.IsValid() && PlaybackRange.IsValid() && !Bindings.ContainsByPredicate([](const auto& V){ return !V.IsValid(); }) && !RootTracks.ContainsByPredicate([](const auto& V){ return !V.IsValid(); }); }
	bool FTimelineAnimationPoseItem::IsValid() const { return !BoneRef.IsEmpty() && Transform.IsValid(); }
	bool FTimelineAnimationMorphWeight::IsValid() const { return !MorphRef.IsEmpty() && FMath::IsFinite(Weight); }

	bool FTimelineAnimationRequestData::IsValidFor(
		const EUeAgentTimelineAnimationRequestKind Kind) const
	{
		switch (Kind)
		{
		case EUeAgentTimelineAnimationRequestKind::SequenceRead:
		case EUeAgentTimelineAnimationRequestKind::MontageRead:
		case EUeAgentTimelineAnimationRequestKind::SkeletonSlotsRead:
		case EUeAgentTimelineAnimationRequestKind::LevelSequenceRead:
		case EUeAgentTimelineAnimationRequestKind::WidgetAnimationRead:
			return !TemporalRange.IsSet() || TemporalRange->IsValid();
		case EUeAgentTimelineAnimationRequestKind::SequenceRootMotionAnalyze:
			return TemporalRange.IsSet() && TemporalRange->IsValid()
				&& SampleRate.IsSet() && SampleRate->IsValid();
		case EUeAgentTimelineAnimationRequestKind::SequencePoseSample:
			return TimePoint.IsSet() && TimePoint->IsValid()
				&& (!OutputFrame.IsSet() || OutputFrame->IsValid());
		case EUeAgentTimelineAnimationRequestKind::BlendSpaceSample:
			return !BlendParameters.IsEmpty()
				&& !BlendParameters.ContainsByPredicate([](const auto& V){ return !V.IsValid(); })
				&& (!TimePoint.IsSet() || TimePoint->IsValid())
				&& (!OutputFrame.IsSet() || OutputFrame->IsValid());
		case EUeAgentTimelineAnimationRequestKind::SequenceBoneTracksReplace:
			return TemporalRange.IsSet() && TemporalRange->IsValid()
				&& SampleRate.IsSet() && SampleRate->IsValid()
				&& ExpectedRevision.IsSet() && ExpectedRevision->IsValid()
				&& !BoneTracks.IsEmpty()
				&& !BoneTracks.ContainsByPredicate([](const auto& V){ return !V.IsValid(); });
		case EUeAgentTimelineAnimationRequestKind::SequenceCurvesUpdate:
			return ExpectedRevision.IsSet() && ExpectedRevision->IsValid()
				&& !CurveChanges.IsEmpty()
				&& !CurveChanges.ContainsByPredicate([](const auto& V){ return !V.IsValid(); });
		case EUeAgentTimelineAnimationRequestKind::SequenceMetadataUpdate:
			return ExpectedRevision.IsSet() && ExpectedRevision->IsValid()
				&& !MetadataChanges.IsEmpty()
				&& !MetadataChanges.ContainsByPredicate([](const auto& V){ return !V.IsValid(); });
		case EUeAgentTimelineAnimationRequestKind::SequenceEventsUpdate:
			return ExpectedRevision.IsSet() && ExpectedRevision->IsValid()
				&& !EventChanges.IsEmpty()
				&& !EventChanges.ContainsByPredicate([](const auto& V){ return !V.IsValid(); });
		case EUeAgentTimelineAnimationRequestKind::MontageCreate:
			return !DestinationRef.IsEmpty() && !SkeletonRef.IsEmpty()
				&& Structure.IsSet() && Structure->IsValid();
		case EUeAgentTimelineAnimationRequestKind::MontageStructureReplace:
		case EUeAgentTimelineAnimationRequestKind::LevelSequenceStructureReplace:
			return Structure.IsSet() && Structure->IsValid()
				&& ExpectedRevision.IsSet() && ExpectedRevision->IsValid();
		case EUeAgentTimelineAnimationRequestKind::LevelSequenceCreate:
			return !DestinationRef.IsEmpty() && Structure.IsSet()
				&& Structure->IsValid()
				&& (!DisplayRate.IsSet() || DisplayRate->IsValid())
				&& (!TickResolution.IsSet() || TickResolution->IsValid());
		case EUeAgentTimelineAnimationRequestKind::LevelSequenceSettingsUpdate:
			return ExpectedRevision.IsSet() && ExpectedRevision->IsValid()
				&& (DisplayRate.IsSet() || TemporalRange.IsSet())
				&& (!DisplayRate.IsSet() || DisplayRate->IsValid())
				&& (!TemporalRange.IsSet() || TemporalRange->IsValid());
		case EUeAgentTimelineAnimationRequestKind::SubjectPoseUpdate:
			return EvaluationPoint.IsSet() && EvaluationPoint->IsValid()
				&& PoseFrame.IsSet() && PoseFrame->IsValid()
				&& !Pose.IsEmpty()
				&& !Pose.ContainsByPredicate([](const auto& V){ return !V.IsValid(); })
				&& (!Weight.IsSet() || (FMath::IsFinite(Weight.GetValue())
					&& Weight.GetValue() >= 0.0 && Weight.GetValue() <= 1.0));
		case EUeAgentTimelineAnimationRequestKind::SubjectMorphWeightsUpdate:
			return EvaluationPoint.IsSet() && EvaluationPoint->IsValid()
				&& !MorphWeights.IsEmpty()
				&& !MorphWeights.ContainsByPredicate([](const auto& V){ return !V.IsValid(); });
		case EUeAgentTimelineAnimationRequestKind::IkProbeCandidateApply:
			return !RigRef.IsEmpty() && !GoalRef.IsEmpty()
				&& Candidate.IsSet() && Candidate->IsValid()
				&& AuthoredTime.IsSet() && AuthoredTime->IsValid()
				&& ExpectedRevision.IsSet() && ExpectedRevision->IsValid();
		default:
			return false;
		}
	}

	FUeAgentTimelineAnimationRequest::FUeAgentTimelineAnimationRequest(
		FString InTypeId,
		FString InFamilyId,
		FString InRequestKindId,
		const EUeAgentTimelineAnimationRequestKind InKind,
		const bool bInOperation,
		FTimelineAnimationRequestData InData,
		FUeAgentCanonicalValueRef InIntentPayload)
		: TypeId(MoveTemp(InTypeId))
		, FamilyId(MoveTemp(InFamilyId))
		, RequestKindId(MoveTemp(InRequestKindId))
		, Kind(InKind)
		, bOperation(bInOperation)
		, Data(MoveTemp(InData))
		, IntentPayload(MoveTemp(InIntentPayload))
	{
	}

	const FString& FUeAgentTimelineAnimationRequest::GetTypeId() const { return TypeId; }
	const FString& FUeAgentTimelineAnimationRequest::GetFamilyId() const { return FamilyId; }
	const FString& FUeAgentTimelineAnimationRequest::GetRequestKindId() const { return RequestKindId; }
	EUeAgentTimelineAnimationRequestKind FUeAgentTimelineAnimationRequest::GetKind() const { return Kind; }
	bool FUeAgentTimelineAnimationRequest::IsOperation() const { return bOperation; }
	const FTimelineAnimationRequestData& FUeAgentTimelineAnimationRequest::GetData() const { return Data; }
	const FUeAgentCanonicalValueRef& FUeAgentTimelineAnimationRequest::GetIntentPayload() const { return IntentPayload; }

	bool FUeAgentTimelineAnimationRequest::TryBuildChangeDraft(
		const FUeAgentCanonicalSnapshot& Snapshot,
		const FUeAgentOperationContext& Context,
		TOptional<FChangeDraft>& OutDraft,
		FUeAgentTimelineAnimationExecutionError& OutError) const
	{
		OutDraft.Reset();
		OutError = {};
		if (!bOperation)
		{
			return Fail(OutError,
				TEXT("timeline_animation_query_has_no_change_draft"), RequestKindId);
		}
		const bool bCreate = Kind == EUeAgentTimelineAnimationRequestKind::MontageCreate
			|| Kind == EUeAgentTimelineAnimationRequestKind::LevelSequenceCreate;
		if (!Data.IsValidFor(Kind)
			|| (!bCreate && Snapshot.GetEntries().IsEmpty()))
		{
			return Fail(OutError,
				TEXT("timeline_animation_preflight_snapshot_invalid"), RequestKindId);
		}
		const FUeAgentCanonicalIdentity PrimaryIdentity = bCreate
			? FUeAgentCanonicalIdentity(
				TEXT("timeline-animation:create:") + RequestKindId,
				TEXT("timeline_animation_target"), Data.DestinationRef,
				Data.DestinationRef, TEXT("target"),
				Snapshot.GetRevision().GetGeneration())
			: Snapshot.GetEntries()[0].GetIdentity();
		TArray<FUeAgentCanonicalIdentity> ImpactTargets;
		if (bCreate)
		{
			ImpactTargets.Add(PrimaryIdentity);
		}
		else for (const FUeAgentStateEntry& Entry : Snapshot.GetEntries())
		{
			ImpactTargets.Add(Entry.GetIdentity());
		}
		FUeAgentCanonicalValuePtr ExpectedBefore;
		if (!bCreate)
		{
			ExpectedBefore = Snapshot.GetEntries()[0].GetValue();
		}
		OutDraft.Emplace(
			PrimaryIdentity,
			bCreate ? EChangeIntent::Create : EChangeIntent::Update,
			MoveTemp(ExpectedBefore),
			IntentPayload,
			TArray<FString>{TEXT("timeline-animation:write")},
			TEXT("timeline-animation-typed-intent:") + RequestKindId,
			MoveTemp(ImpactTargets));
		return OutDraft->IsValid();
	}

	bool TryCreateTimelineAnimationRequestDecoder(
		TSharedPtr<IUeAgentTimelineAnimationRequestDecoder>& OutDecoder,
		FUeAgentTimelineAnimationExecutionError& OutError)
	{
		OutDecoder.Reset();
		OutError = {};
		if (RequestSpecs().Num() != 20)
		{
			return Fail(OutError,
				TEXT("timeline_animation_request_spec_count_invalid"));
		}
		TSet<FString> Capabilities;
		TSet<FString> Types;
		for (const FRequestSpec& Spec : RequestSpecs())
		{
			if (Capabilities.Contains(Spec.CapabilityId)
				|| Types.Contains(Spec.TypeId))
			{
				return Fail(OutError,
					TEXT("timeline_animation_request_spec_duplicate"), Spec.CapabilityId);
			}
			Capabilities.Add(Spec.CapabilityId);
			Types.Add(Spec.TypeId);
		}
		OutDecoder = MakeShared<FTimelineAnimationRequestDecoder>();
		return true;
	}
}
