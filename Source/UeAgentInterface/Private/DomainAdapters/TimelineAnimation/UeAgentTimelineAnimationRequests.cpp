#include "DomainAdapters/TimelineAnimation/UeAgentTimelineAnimationRequests.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Utilities/AuthoringCore/UeAgentTimelineValueJson.h"
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
		using namespace Authoring::Timeline;
		using namespace Authoring::Timeline::Json;
















		FDiagnostic TimelineRequestFact(const FString& Code, const FString& Reason, const FString& Path,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
		{
			auto Error = FDiagnostic::Failure(Code, Reason, TEXT("native_execution"));
			Error.Schema = TEXT("uai.runtime.diagnostic/1");
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = Path;
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			return Error;
		}

		bool ParseRevision(const TSharedPtr<FJsonObject>& Root, FTimelineAnimationRevision& Out,
			FUeAgentTimelineAnimationExecutionError& Error)
		{
			Out = {};
			FString SequenceLexeme;
			if (!RequiredString(Root, TEXT("source_id"), Out.SourceId, Error)
				|| !RequiredString(Root, TEXT("generation"), Out.Generation, Error)
				|| !RequiredString(Root, TEXT("sequence"), SequenceLexeme, Error)) return false;
			if (!LexTryParseString(Out.Sequence, *SequenceLexeme)
				|| FString::Printf(TEXT("%lld"), Out.Sequence) != SequenceLexeme)
				return FieldFailure(Root, TEXT("sequence"), TEXT("timeline_animation_revision_invalid"),
					TEXT("a canonical decimal int64 string"), Error);
			if (Out.SourceId.IsEmpty())
				return FieldFailure(Root, TEXT("source_id"), TEXT("timeline_animation_revision_invalid"), TEXT("a non-empty revision source id"), Error);
			if (Out.Generation.IsEmpty())
				return FieldFailure(Root, TEXT("generation"), TEXT("timeline_animation_revision_invalid"), TEXT("a non-empty revision generation"), Error);
			if (Out.Sequence < 0)
				return FieldFailure(Root, TEXT("sequence"), TEXT("timeline_animation_revision_invalid"), TEXT("a non-negative canonical decimal int64 string"), Error);
			return true;
		}















		bool ParseBoneKey(const TSharedPtr<FJsonObject>& Root,
			FTimelineAnimationBoneKey& Out,
			FUeAgentTimelineAnimationExecutionError& Error)
		{
			TSharedPtr<FJsonObject> Time;
			TSharedPtr<FJsonObject> Transform;
			if (!RequiredObject(Root, TEXT("time"), Time, Error) || !RequiredObject(Root, TEXT("transform"), Transform, Error)) return false;
			if (!ParsePoint(Time, Out.Time, Error)) return PrefixChildFailure(Root, TEXT("/time"), Error);
			if (!ParseTransform(Transform, Out.Transform, Error)) return PrefixChildFailure(Root, TEXT("/transform"), Error);
			return true;
		}

		bool ParseBoneTrack(const TSharedPtr<FJsonObject>& Root,
			FTimelineAnimationBoneTrack& Out,
			FUeAgentTimelineAnimationExecutionError& Error)
		{
			if (!RequiredString(Root, TEXT("bone_ref"), Out.BoneRef, Error)
				|| !ParseObjectArray(Root, TEXT("keys"), Out.Keys, true, ParseBoneKey, Error)) return false;
			if (Out.BoneRef.IsEmpty())
				return FieldFailure(Root, TEXT("bone_ref"), TEXT("timeline_animation_bone_track_invalid"), TEXT("a non-empty bone reference"), Error);
			if (Out.Keys.IsEmpty())
				return FieldFailure(Root, TEXT("keys"), TEXT("timeline_animation_bone_track_invalid"), TEXT("at least one bone key"), Error);
			return true;
		}

		bool ParseBlendParameter(const TSharedPtr<FJsonObject>& Root,
			FTimelineAnimationBlendParameter& Out,
			FUeAgentTimelineAnimationExecutionError& Error)
		{
			if (!RequiredString(Root, TEXT("axis_ref"), Out.AxisRef, Error)) return false;
			if (!Root->TryGetNumberField(TEXT("value"), Out.Value))
				return FieldFailure(Root, TEXT("value"), TEXT("timeline_animation_blend_parameter_invalid"), TEXT("a number"), Error);
			if (Out.AxisRef.IsEmpty())
				return FieldFailure(Root, TEXT("axis_ref"), TEXT("timeline_animation_blend_parameter_invalid"), TEXT("a non-empty blend axis reference"), Error);
			if (!FMath::IsFinite(Out.Value))
				return FieldFailure(Root, TEXT("value"), TEXT("timeline_animation_blend_parameter_invalid"), TEXT("a finite number"), Error);
			return true;
		}













		bool ParsePoseItem(const TSharedPtr<FJsonObject>& Root,
			FTimelineAnimationPoseItem& Out,
			FUeAgentTimelineAnimationExecutionError& Error)
		{
			TSharedPtr<FJsonObject> Transform;
			if (!RequiredString(Root, TEXT("bone_ref"), Out.BoneRef, Error) || !RequiredObject(Root, TEXT("transform"), Transform, Error)) return false;
			if (!ParseTransform(Transform, Out.Transform, Error)) return PrefixChildFailure(Root, TEXT("/transform"), Error);
			if (Out.BoneRef.IsEmpty())
				return FieldFailure(Root, TEXT("bone_ref"), TEXT("timeline_animation_pose_item_invalid"), TEXT("a non-empty bone reference"), Error);
			return true;
		}

		bool ParseMorphWeight(const TSharedPtr<FJsonObject>& Root,
			FTimelineAnimationMorphWeight& Out,
			FUeAgentTimelineAnimationExecutionError& Error)
		{
			if (!RequiredString(Root, TEXT("morph_ref"), Out.MorphRef, Error)) return false;
			if (!Root->TryGetNumberField(TEXT("weight"), Out.Weight))
				return FieldFailure(Root, TEXT("weight"), TEXT("timeline_animation_morph_weight_invalid"), TEXT("a number"), Error);
			if (Out.MorphRef.IsEmpty())
				return FieldFailure(Root, TEXT("morph_ref"), TEXT("timeline_animation_morph_weight_invalid"), TEXT("a non-empty morph reference"), Error);
			if (!FMath::IsFinite(Out.Weight))
				return FieldFailure(Root, TEXT("weight"), TEXT("timeline_animation_morph_weight_invalid"), TEXT("a finite number"), Error);
			return true;
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
				return ValueFailure(nullptr, TEXT("/"), TEXT("timeline_animation_intent_json_invalid"), TEXT("a JSON value"), nullptr, Error);
			}
			FUeAgentStateSemanticError StateError;
			switch (JsonValue->Type)
			{
			case EJson::Null:
				return FUeAgentCanonicalValue::TryNull(Resources, OutValue, StateError)
					|| Fail(Error, StateError);
			case EJson::Boolean:
				return FUeAgentCanonicalValue::TryBoolean(
					JsonValue->AsBool(), Resources, OutValue, StateError)
					|| Fail(Error, StateError);
			case EJson::String:
				return FUeAgentCanonicalValue::TryString(
					JsonValue->AsString(), Resources, OutValue, StateError)
					|| Fail(Error, StateError);
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
					|| Fail(Error, StateError);
			}
			case EJson::Array:
			{
				TArray<FUeAgentCanonicalValueRef> Items;
				for (int32 Index = 0; Index < JsonValue->AsArray().Num(); ++Index)
				{
					const auto& Item = JsonValue->AsArray()[Index];
					FUeAgentCanonicalValuePtr Canonical;
					if (!ConvertCanonicalValue(Item, Resources, Canonical, Error))
						return PrefixChildFailure(nullptr, TEXT("/") + FString::FromInt(Index), Error);
					Items.Add(Canonical.ToSharedRef());
				}
				return FUeAgentCanonicalValue::TryArray(
					Items, Resources, OutValue, StateError)
					|| Fail(Error, StateError);
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
						return PrefixChildFailure(nullptr, TEXT("/") + Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")), Error);
					}
					Items.Emplace(Key, Canonical.ToSharedRef());
				}
				return FUeAgentCanonicalValue::TryObject(
					Items, Resources, OutValue, StateError)
					|| Fail(Error, StateError);
			}
			default:
				return ValueFailure(nullptr, TEXT("/"), TEXT("timeline_animation_intent_json_kind_invalid"), TEXT("null, boolean, string, number, array, or object"),
					MakeShared<FJsonValueNumber>(static_cast<int32>(JsonValue->Type)), Error);
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
			if (!ParseRange(Object, Value, Error)) return PrefixChildFailure(Root, TEXT("/") + FString(Field), Error);
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
			if (!ParsePoint(Object, Value, Error)) return PrefixChildFailure(Root, TEXT("/") + FString(Field), Error);
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
			if (!ParseRate(Object, Value, Error)) return PrefixChildFailure(Root, TEXT("/") + FString(Field), Error);
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
			if (!RequiredObject(Root, TEXT("expected_revision"), Object, Error)) return false;
			if (!ParseRevision(Object, Value, Error)) return PrefixChildFailure(Root, TEXT("/expected_revision"), Error);
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
					if (!ParseRange(Object, Value, Error)) return PrefixChildFailure(Root, TEXT("/temporal_range"), Error);
					Data.TemporalRange = MoveTemp(Value);
				}
				if (!RequiredObject(Root, TEXT("sample_rate"), Object, Error)) return false;
				{
					FTimelineAnimationRate Value;
					if (!ParseRate(Object, Value, Error)) return PrefixChildFailure(Root, TEXT("/sample_rate"), Error);
					Data.SampleRate = MoveTemp(Value);
				}
				return OptionalString(Root, TEXT("root_bone_ref"), Data.RootBoneRef, Error);
			case EUeAgentTimelineAnimationRequestKind::SequencePoseSample:
				if (!RequiredObject(Root, TEXT("time_point"), Object, Error)) return false;
				{
					FTimelineAnimationPoint Value;
					if (!ParsePoint(Object, Value, Error)) return PrefixChildFailure(Root, TEXT("/time_point"), Error);
					Data.TimePoint = MoveTemp(Value);
				}
				if (!ParseStringArray(Root, TEXT("bone_refs"), Data.BoneRefs, false, Error)) return false;
				if (!OptionalObject(Root, TEXT("output_frame"), Object, bPresent, Error)) return false;
				if (bPresent)
				{
					FTimelineAnimationFrame3d Frame;
					if (!ParseFrame(Object, Frame, Error)) return PrefixChildFailure(Root, TEXT("/output_frame"), Error);
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
					if (!ParseFrame(Object, Frame, Error)) return PrefixChildFailure(Root, TEXT("/output_frame"), Error);
					Data.OutputFrame = MoveTemp(Frame);
				}
				return true;
			case EUeAgentTimelineAnimationRequestKind::SequenceBoneTracksReplace:
				if (!RequiredObject(Root, TEXT("authored_range"), Object, Error)) return false;
				{
					FTimelineAnimationRange Value;
					if (!ParseRange(Object, Value, Error)) return PrefixChildFailure(Root, TEXT("/authored_range"), Error);
					Data.TemporalRange = MoveTemp(Value);
				}
				if (!RequiredObject(Root, TEXT("sample_rate"), Object, Error)) return false;
				{
					FTimelineAnimationRate Value;
					if (!ParseRate(Object, Value, Error)) return PrefixChildFailure(Root, TEXT("/sample_rate"), Error);
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
					if (!ParseStructure(Object, Structure, Error)) return PrefixChildFailure(Root, TEXT("/initial_structure"), Error);
					Data.Structure = MoveTemp(Structure);
				}
				return OptionalString(Root, TEXT("source_animation_ref"), Data.SourceAnimationRef, Error);
			case EUeAgentTimelineAnimationRequestKind::MontageStructureReplace:
				if (!RequiredObject(Root, TEXT("structure"), Object, Error)) return false;
				{
					FTimelineAnimationStructure Structure;
					if (!ParseStructure(Object, Structure, Error)) return PrefixChildFailure(Root, TEXT("/structure"), Error);
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
					if (!ParseStructure(Object, Structure, Error)) return PrefixChildFailure(Root, TEXT("/initial_structure"), Error);
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
					if (!ParseStructure(Object, Structure, Error)) return PrefixChildFailure(Root, TEXT("/structure"), Error);
					Data.Structure = MoveTemp(Structure);
				}
				return ParseRequiredRevision(Root, Data, Error);
			case EUeAgentTimelineAnimationRequestKind::WidgetAnimationRead:
				return ParseOptionalRange(Root, TEXT("temporal_range"), Data.TemporalRange, Error)
					&& OptionalBool(Root, TEXT("include_bindings"), Data.IncludeBindings, Error)
					&& OptionalBool(Root, TEXT("include_tracks"), Data.IncludeTracks, Error);
			case EUeAgentTimelineAnimationRequestKind::SubjectPoseUpdate:
				if (!ParseOptionalPoint(Root, TEXT("evaluation_point"), Data.EvaluationPoint, Error)) return false;
				if (!Data.EvaluationPoint.IsSet())
					return FieldFailure(Root, TEXT("evaluation_point"), TEXT("timeline_animation_evaluation_point_required"), TEXT("an evaluation point"), Error);
				if (!ParseObjectArray(Root, TEXT("pose"), Data.Pose, true, ParsePoseItem, Error)
					|| !RequiredObject(Root, TEXT("pose_frame"), Object, Error)) return false;
				{
					FTimelineAnimationFrame3d Frame;
					if (!ParseFrame(Object, Frame, Error)) return PrefixChildFailure(Root, TEXT("/pose_frame"), Error);
					Data.PoseFrame = MoveTemp(Frame);
				}
				return OptionalNumber(Root, TEXT("weight"), Data.Weight, Error);
			case EUeAgentTimelineAnimationRequestKind::SubjectMorphWeightsUpdate:
				if (!ParseOptionalPoint(Root, TEXT("evaluation_point"), Data.EvaluationPoint, Error)) return false;
				if (!Data.EvaluationPoint.IsSet())
					return FieldFailure(Root, TEXT("evaluation_point"), TEXT("timeline_animation_evaluation_point_required"), TEXT("an evaluation point"), Error);
				return ParseObjectArray(Root, TEXT("morph_weights"), Data.MorphWeights, true, ParseMorphWeight, Error);
			case EUeAgentTimelineAnimationRequestKind::IkProbeCandidateApply:
				if (!RequiredString(Root, TEXT("rig_ref"), Data.RigRef, Error)
					|| !RequiredString(Root, TEXT("goal_ref"), Data.GoalRef, Error)
					|| !RequiredObject(Root, TEXT("candidate"), Object, Error)) return false;
				{
					FTimelineAnimationTransform3d Transform;
					if (!ParseTransform(Object, Transform, Error)) return PrefixChildFailure(Root, TEXT("/candidate"), Error);
					Data.Candidate = MoveTemp(Transform);
				}
				if (!RequiredObject(Root, TEXT("authored_time"), Object, Error)) return false;
				{
					FTimelineAnimationPoint Point;
					if (!ParsePoint(Object, Point, Error)) return PrefixChildFailure(Root, TEXT("/authored_time"), Error);
					Data.AuthoredTime = MoveTemp(Point);
				}
				return ParseRequiredRevision(Root, Data, Error);
			default:
				return ValueFailure(Root, TEXT("/request_kind"), TEXT("timeline_animation_request_kind_invalid"),
					TEXT("a registered timeline request kind"), MakeShared<FJsonValueNumber>(static_cast<int32>(Kind)), Error);
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
				if (!Spec)
					return Fail(OutError, TimelineRequestFact(TEXT("timeline_animation_request_contract_mismatch"),
						TEXT("No typed timeline request specification is registered for the capability."), TEXT("/contract/capability_id"),
						MakeShared<FJsonValueString>(Contract.GetCapabilityId()), MakeShared<FJsonValueString>(TEXT("registered timeline capability"))));
				if (Contract.GetTypedRequestId() != Spec->TypeId)
					return Fail(OutError, TimelineRequestFact(TEXT("timeline_animation_request_contract_mismatch"),
						TEXT("The execution contract requests a different typed request id."), TEXT("/contract/typed_request_id"),
						MakeShared<FJsonValueString>(Contract.GetTypedRequestId()), MakeShared<FJsonValueString>(Spec->TypeId)));
				if (Contract.GetFamilyId() != Spec->FamilyId)
					return Fail(OutError, TimelineRequestFact(TEXT("timeline_animation_request_contract_mismatch"),
						TEXT("The execution contract uses a different timeline port family."), TEXT("/contract/family_id"),
						MakeShared<FJsonValueString>(Contract.GetFamilyId()), MakeShared<FJsonValueString>(Spec->FamilyId)));
				if (Contract.RequiresNativeMutation() != Spec->bOperation)
					return Fail(OutError, TimelineRequestFact(TEXT("timeline_animation_request_contract_mismatch"),
						TEXT("The execution contract mutation flag differs from the typed request specification."), TEXT("/contract/requires_native_mutation"),
						MakeShared<FJsonValueBoolean>(Contract.RequiresNativeMutation()), MakeShared<FJsonValueBoolean>(Spec->bOperation)));
				TSharedPtr<FJsonObject> Root;
				UE::UAI::Contract::FDiagnostic JsonError;
				if (!UE::UAI::Contract::Json::TryParseObject(CanonicalInput.GetJsonValue(), Root, JsonError)
					|| !Root.IsValid())
				{
					return Fail(OutError, JsonError.IsEmpty() ? TimelineRequestFact(TEXT("timeline_animation_input_object_missing"),
						TEXT("Canonical timeline input did not yield an object."), TEXT("/input"),
						MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("object"))) : MoveTemp(JsonError));
				}
				FTimelineAnimationRequestData Data;
				if (!DecodeData(Spec->Kind, Root, Data, OutError) || !Data.TryValidateFor(Spec->Kind, OutError))
				{
					if (OutError.IsEmpty()) OutError = TimelineRequestFact(TEXT("timeline_animation_request_data_invalid"),
						TEXT("The typed timeline request parser failed without returning a diagnostic."), TEXT("/request_parser/succeeded"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return false;
				}
				FUeAgentCanonicalValuePtr Intent;
				if (!ConvertCanonicalValue(
						MakeShared<FJsonValueObject>(Root), Resources, Intent, OutError)
					|| !Intent.IsValid())
				{
					if (OutError.IsEmpty()) OutError = TimelineRequestFact(TEXT("timeline_animation_intent_value_missing"),
						TEXT("Canonical intent conversion returned no value."), TEXT("/intent_payload"),
						MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("canonical value")));
					return false;
				}
				OutRequest = MakeShared<FUeAgentTimelineAnimationRequest>(
					Spec->TypeId, Spec->FamilyId, Spec->RequestKindId,
					Spec->Kind, Spec->bOperation, MoveTemp(Data), Intent.ToSharedRef());
				return true;
			}
		};
	}

	bool FTimelineAnimationRevision::TryValidate(FUeAgentTimelineAnimationExecutionError& Error) const
	{
		Error = {};
		if (SourceId.IsEmpty()) return ValueFailure(nullptr, TEXT("/source_id"), TEXT("timeline_animation_revision_invalid"), TEXT("a non-empty revision source id"),
			MakeShared<FJsonValueString>(SourceId), Error);
		if (Generation.IsEmpty()) return ValueFailure(nullptr, TEXT("/generation"), TEXT("timeline_animation_revision_invalid"), TEXT("a non-empty revision generation"),
			MakeShared<FJsonValueString>(Generation), Error);
		return Sequence >= 0 || ValueFailure(nullptr, TEXT("/sequence"), TEXT("timeline_animation_revision_invalid"), TEXT("a non-negative sequence"),
			MakeShared<FJsonValueString>(FString::Printf(TEXT("%lld"), Sequence)), Error);
	}

	bool FTimelineAnimationRevision::IsValid() const { FUeAgentTimelineAnimationExecutionError Error; return TryValidate(Error); }

















	bool FTimelineAnimationBoneKey::TryValidate(FUeAgentTimelineAnimationExecutionError& Error) const
	{
		Error = {};
		if (!Time.TryValidate(Error)) return PrefixChildFailure(nullptr, TEXT("/time"), Error);
		return Transform.TryValidate(Error) || PrefixChildFailure(nullptr, TEXT("/transform"), Error);
	}

	bool FTimelineAnimationBoneTrack::TryValidate(FUeAgentTimelineAnimationExecutionError& Error) const
	{
		Error = {};
		if (BoneRef.IsEmpty()) return ValueFailure(nullptr, TEXT("/bone_ref"), TEXT("timeline_animation_bone_track_invalid"),
			TEXT("a non-empty bone reference"), MakeShared<FJsonValueString>(BoneRef), Error);
		if (Keys.IsEmpty())
		{
			auto Shape = MakeShared<FJsonObject>();
			Shape->SetNumberField(TEXT("count"), 0);
			return ValueFailure(nullptr, TEXT("/keys"), TEXT("timeline_animation_bone_track_invalid"), TEXT("at least one bone key"), MakeShared<FJsonValueObject>(Shape), Error);
		}
		for (int32 Index = 0; Index < Keys.Num(); ++Index)
			if (!Keys[Index].TryValidate(Error)) return PrefixChildFailure(nullptr, TEXT("/keys/") + FString::FromInt(Index), Error);
		return true;
	}

	bool FTimelineAnimationBlendParameter::TryValidate(FUeAgentTimelineAnimationExecutionError& Error) const
	{
		Error = {};
		if (AxisRef.IsEmpty()) return ValueFailure(nullptr, TEXT("/axis_ref"), TEXT("timeline_animation_blend_parameter_invalid"),
			TEXT("a non-empty blend axis reference"), MakeShared<FJsonValueString>(AxisRef), Error);
		return FMath::IsFinite(Value) || ValueFailure(nullptr, TEXT("/value"), TEXT("timeline_animation_blend_parameter_invalid"),
			TEXT("a finite number"), MakeShared<FJsonValueString>(FString::SanitizeFloat(Value)), Error);
	}

	bool FTimelineAnimationBoneKey::IsValid() const { FUeAgentTimelineAnimationExecutionError Error; return TryValidate(Error); }
	bool FTimelineAnimationBoneTrack::IsValid() const { FUeAgentTimelineAnimationExecutionError Error; return TryValidate(Error); }
	bool FTimelineAnimationBlendParameter::IsValid() const { FUeAgentTimelineAnimationExecutionError Error; return TryValidate(Error); }







	bool TryParseTimelineAnimationStructure(
		const TSharedPtr<FJsonObject>& Root,
		FTimelineAnimationStructure& OutStructure,
		FUeAgentTimelineAnimationExecutionError& OutError)
	{
		OutStructure = {};
		OutError = {};
		return Root.IsValid()
			? ParseStructure(Root, OutStructure, OutError)
			: ValueFailure(nullptr, TEXT("/structure"), TEXT("timeline_animation_structure_missing"), TEXT("a structure object"), nullptr, OutError);
	}
	bool FTimelineAnimationPoseItem::TryValidate(FUeAgentTimelineAnimationExecutionError& Error) const
	{
		Error = {};
		if (BoneRef.IsEmpty()) return ValueFailure(nullptr, TEXT("/bone_ref"), TEXT("timeline_animation_pose_item_invalid"),
			TEXT("a non-empty bone reference"), MakeShared<FJsonValueString>(BoneRef), Error);
		return Transform.TryValidate(Error) || PrefixChildFailure(nullptr, TEXT("/transform"), Error);
	}

	bool FTimelineAnimationMorphWeight::TryValidate(FUeAgentTimelineAnimationExecutionError& Error) const
	{
		Error = {};
		if (MorphRef.IsEmpty()) return ValueFailure(nullptr, TEXT("/morph_ref"), TEXT("timeline_animation_morph_weight_invalid"),
			TEXT("a non-empty morph reference"), MakeShared<FJsonValueString>(MorphRef), Error);
		return FMath::IsFinite(Weight) || ValueFailure(nullptr, TEXT("/weight"), TEXT("timeline_animation_morph_weight_invalid"),
			TEXT("a finite number"), MakeShared<FJsonValueString>(FString::SanitizeFloat(Weight)), Error);
	}

	bool FTimelineAnimationPoseItem::IsValid() const { FUeAgentTimelineAnimationExecutionError Error; return TryValidate(Error); }
	bool FTimelineAnimationMorphWeight::IsValid() const { FUeAgentTimelineAnimationExecutionError Error; return TryValidate(Error); }

	bool FTimelineAnimationRequestData::TryValidateFor(const EUeAgentTimelineAnimationRequestKind Kind,
		FUeAgentTimelineAnimationExecutionError& Error) const
	{
		Error = {};
		const auto Required = [&](const auto& Optional, const TCHAR* Field)
		{
			const FString Path = TEXT("/") + FString(Field);
			if (!Optional.IsSet()) return ValueFailure(nullptr, Path, TEXT("timeline_animation_request_field_required"), TEXT("a required value"), nullptr, Error);
			return Optional->TryValidate(Error) || PrefixChildFailure(nullptr, Path, Error);
		};
		const auto Optional = [&](const auto& Value, const TCHAR* Field) { return !Value.IsSet() || Required(Value, Field); };
		const auto Array = [&](const auto& Values, const TCHAR* Field)
		{
			const FString Path = TEXT("/") + FString(Field);
			if (Values.IsEmpty())
			{
				auto Shape = MakeShared<FJsonObject>();
				Shape->SetNumberField(TEXT("count"), 0);
				return ValueFailure(nullptr, Path, TEXT("timeline_animation_request_array_empty"), TEXT("at least one item"), MakeShared<FJsonValueObject>(Shape), Error);
			}
			for (int32 Index = 0; Index < Values.Num(); ++Index)
				if (!Values[Index].TryValidate(Error)) return PrefixChildFailure(nullptr, Path + TEXT("/") + FString::FromInt(Index), Error);
			return true;
		};
		const auto Text = [&](const FString& Value, const TCHAR* Field)
		{
			return !Value.IsEmpty() || ValueFailure(nullptr, TEXT("/") + FString(Field), TEXT("timeline_animation_request_field_required"),
				TEXT("a non-empty string"), MakeShared<FJsonValueString>(Value), Error);
		};
		switch (Kind)
		{
		case EUeAgentTimelineAnimationRequestKind::SequenceRead:
		case EUeAgentTimelineAnimationRequestKind::MontageRead:
		case EUeAgentTimelineAnimationRequestKind::SkeletonSlotsRead:
		case EUeAgentTimelineAnimationRequestKind::LevelSequenceRead:
		case EUeAgentTimelineAnimationRequestKind::WidgetAnimationRead:
			return Optional(TemporalRange, TEXT("temporal_range"));
		case EUeAgentTimelineAnimationRequestKind::SequenceRootMotionAnalyze:
			return Required(TemporalRange, TEXT("temporal_range")) && Required(SampleRate, TEXT("sample_rate"));
		case EUeAgentTimelineAnimationRequestKind::SequencePoseSample:
			return Required(TimePoint, TEXT("time_point")) && Optional(OutputFrame, TEXT("output_frame"));
		case EUeAgentTimelineAnimationRequestKind::BlendSpaceSample:
			return Array(BlendParameters, TEXT("blend_parameters")) && Optional(TimePoint, TEXT("time_point")) && Optional(OutputFrame, TEXT("output_frame"));
		case EUeAgentTimelineAnimationRequestKind::SequenceBoneTracksReplace:
			return Required(TemporalRange, TEXT("authored_range")) && Required(SampleRate, TEXT("sample_rate"))
				&& Required(ExpectedRevision, TEXT("expected_revision")) && Array(BoneTracks, TEXT("bone_tracks"));
		case EUeAgentTimelineAnimationRequestKind::SequenceCurvesUpdate:
			return Required(ExpectedRevision, TEXT("expected_revision")) && Array(CurveChanges, TEXT("curve_changes"));
		case EUeAgentTimelineAnimationRequestKind::SequenceMetadataUpdate:
			return Required(ExpectedRevision, TEXT("expected_revision")) && Array(MetadataChanges, TEXT("metadata_changes"));
		case EUeAgentTimelineAnimationRequestKind::SequenceEventsUpdate:
			return Required(ExpectedRevision, TEXT("expected_revision")) && Array(EventChanges, TEXT("event_changes"));
		case EUeAgentTimelineAnimationRequestKind::MontageCreate:
			return Text(DestinationRef, TEXT("destination_ref")) && Text(SkeletonRef, TEXT("skeleton_ref")) && Required(Structure, TEXT("initial_structure"));
		case EUeAgentTimelineAnimationRequestKind::MontageStructureReplace:
		case EUeAgentTimelineAnimationRequestKind::LevelSequenceStructureReplace:
			return Required(Structure, TEXT("structure")) && Required(ExpectedRevision, TEXT("expected_revision"));
		case EUeAgentTimelineAnimationRequestKind::LevelSequenceCreate:
			return Text(DestinationRef, TEXT("destination_ref")) && Required(Structure, TEXT("initial_structure"))
				&& Optional(DisplayRate, TEXT("display_rate")) && Optional(TickResolution, TEXT("tick_resolution"));
		case EUeAgentTimelineAnimationRequestKind::LevelSequenceSettingsUpdate:
			if (!Required(ExpectedRevision, TEXT("expected_revision"))) return false;
			if (!DisplayRate.IsSet() && !TemporalRange.IsSet())
			{
				auto Presence = MakeShared<FJsonObject>();
				Presence->SetBoolField(TEXT("display_rate_present"), false);
				Presence->SetBoolField(TEXT("playback_range_present"), false);
				return ValueFailure(nullptr, TEXT("/"), TEXT("timeline_animation_settings_update_empty"),
					TEXT("display_rate or playback_range must be present"), MakeShared<FJsonValueObject>(Presence), Error);
			}
			return Optional(DisplayRate, TEXT("display_rate")) && Optional(TemporalRange, TEXT("playback_range"));
		case EUeAgentTimelineAnimationRequestKind::SubjectPoseUpdate:
			if (!Required(EvaluationPoint, TEXT("evaluation_point")) || !Required(PoseFrame, TEXT("pose_frame")) || !Array(Pose, TEXT("pose"))) return false;
			if (Weight.IsSet() && (!FMath::IsFinite(Weight.GetValue()) || Weight.GetValue() < 0.0 || Weight.GetValue() > 1.0))
				return ValueFailure(nullptr, TEXT("/weight"), TEXT("timeline_animation_pose_weight_invalid"), TEXT("a finite number in [0, 1]"),
					FMath::IsFinite(Weight.GetValue()) ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(Weight.GetValue()))
						: TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(FString::SanitizeFloat(Weight.GetValue()))), Error);
			return true;
		case EUeAgentTimelineAnimationRequestKind::SubjectMorphWeightsUpdate:
			return Required(EvaluationPoint, TEXT("evaluation_point")) && Array(MorphWeights, TEXT("morph_weights"));
		case EUeAgentTimelineAnimationRequestKind::IkProbeCandidateApply:
			return Text(RigRef, TEXT("rig_ref")) && Text(GoalRef, TEXT("goal_ref")) && Required(Candidate, TEXT("candidate"))
				&& Required(AuthoredTime, TEXT("authored_time")) && Required(ExpectedRevision, TEXT("expected_revision"));
		default:
			return ValueFailure(nullptr, TEXT("/request_kind"), TEXT("timeline_animation_request_kind_invalid"), TEXT("a registered timeline request kind"),
				MakeShared<FJsonValueNumber>(static_cast<int32>(Kind)), Error);
		}
	}

	bool FTimelineAnimationRequestData::IsValidFor(const EUeAgentTimelineAnimationRequestKind Kind) const
	{
		FUeAgentTimelineAnimationExecutionError Error;
		return TryValidateFor(Kind, Error);
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
			return Fail(OutError, TimelineRequestFact(TEXT("timeline_animation_query_has_no_change_draft"),
				TEXT("A query request cannot produce a mutation change draft."), TEXT("/request/is_operation"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
		}
		const bool bCreate = Kind == EUeAgentTimelineAnimationRequestKind::MontageCreate
			|| Kind == EUeAgentTimelineAnimationRequestKind::LevelSequenceCreate;
		if (!Data.TryValidateFor(Kind, OutError)) return false;
		if (!bCreate && Snapshot.GetEntries().IsEmpty())
			return Fail(OutError, TimelineRequestFact(TEXT("timeline_animation_preflight_snapshot_invalid"),
				TEXT("Updating an existing timeline target requires a non-empty preflight snapshot."), TEXT("/snapshot/entries"),
				MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("at least one entry"))));
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
		if (!OutDraft->IsValid()) return Fail(OutError, OutDraft->GetValidationDiagnostic());
		return true;
	}

	bool TryCreateTimelineAnimationRequestDecoder(
		TSharedPtr<IUeAgentTimelineAnimationRequestDecoder>& OutDecoder,
		FUeAgentTimelineAnimationExecutionError& OutError)
	{
		OutDecoder.Reset();
		OutError = {};
		TSet<FString> Capabilities;
		TSet<FString> Types;
		for (const FRequestSpec& Spec : RequestSpecs())
		{
			if (Capabilities.Contains(Spec.CapabilityId)
				|| Types.Contains(Spec.TypeId))
			{
				const bool bCapabilityDuplicate = Capabilities.Contains(Spec.CapabilityId);
				return Fail(OutError, TimelineRequestFact(TEXT("timeline_animation_request_spec_duplicate"),
					bCapabilityDuplicate ? TEXT("A typed request capability is registered more than once.") : TEXT("A typed request id is registered more than once."),
					TEXT("/request_specs/") + FString::FromInt(Capabilities.Num()) + (bCapabilityDuplicate ? TEXT("/capability_id") : TEXT("/type_id")),
					MakeShared<FJsonValueString>(bCapabilityDuplicate ? Spec.CapabilityId : Spec.TypeId), MakeShared<FJsonValueString>(TEXT("unique registration"))));
			}
			Capabilities.Add(Spec.CapabilityId);
			Types.Add(Spec.TypeId);
		}
		OutDecoder = MakeShared<FTimelineAnimationRequestDecoder>();
		return true;
	}
}
