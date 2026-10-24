#include "Utilities/AuthoringCore/UeAgentTimelineValueTypes.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Utilities/AuthoringCore/UeAgentTimelineValueJson.h"
#include "Core/HjsonWorkspace/UeAgentKeyedItemAuthoringProjection.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Misc/ScopeExit.h"

namespace UE::UAI::Authoring::Timeline
{
	using namespace StateSemantic;
	using namespace Json;

	bool ParseRate(
		const TSharedPtr<FJsonObject>& Root,
		FTimelineAnimationRate& Out,
		FUeAgentTimelineAnimationExecutionError& Error)
	{
		ON_SCOPE_EXIT { if (!Error.IsEmpty() && Root) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Root); };
		Out = {};
		if (!RequiredString(Root, TEXT("numerator"), Out.Numerator, Error)
			|| !RequiredString(Root, TEXT("denominator"), Out.Denominator, Error)) return false;
		const FUeAgentTemporalRate Rate(Out.Numerator, Out.Denominator);
		if (!Rate.IsValid())
		{
			auto Cause = Rate.GetValidationDiagnostic();
			Cause.SemanticPointer = TEXT("/") + Cause.PropertyPath;
			UeAgentAuthoringDiagnostics::LocateAuthoringSource(Cause, *Root, Cause.PropertyPath);
			Error = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("timeline_animation_rate_invalid"), Cause);
			return false;
		}
		return true;
	}

	bool ParsePoint(
		const TSharedPtr<FJsonObject>& Root,
		FTimelineAnimationPoint& Out,
		FUeAgentTimelineAnimationExecutionError& Error)
	{
		ON_SCOPE_EXIT { if (!Error.IsEmpty() && Root) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Root); };
		Out = {};
		TSharedPtr<FJsonObject> Rate;
		if (!RequiredString(Root, TEXT("clock_id"), Out.ClockId, Error)
			|| !RequiredString(Root, TEXT("tick"), Out.Tick, Error)
			|| !RequiredObject(Root, TEXT("rate"), Rate, Error)) return false;
		if (!ParseRate(Rate, Out.Rate, Error)) return PrefixChildFailure(Root, TEXT("/rate"), Error);
		if (!RequiredString(Root, TEXT("state"), Out.State, Error)) return false;
		if (Out.State != TEXT("authored")) return FieldFailure(Root, TEXT("state"), TEXT("timeline_animation_point_invalid"), TEXT("authored"), Error);
		const auto Point = Out.ToTemporalPoint();
		if (!Point.IsValid())
		{
			auto Cause = Point.GetValidationDiagnostic();
			Cause.SemanticPointer = TEXT("/") + Cause.PropertyPath;
			UeAgentAuthoringDiagnostics::LocateAuthoringSource(Cause, *Root, Cause.PropertyPath);
			Error = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("timeline_animation_point_invalid"), Cause);
			return false;
		}
		return true;
	}

	bool ParseBound(
		const TSharedPtr<FJsonObject>& Root,
		FTimelineAnimationBound& Out,
		FUeAgentTimelineAnimationExecutionError& Error)
	{
		ON_SCOPE_EXIT { if (!Error.IsEmpty() && Root) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Root); };
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
				return PrefixChildFailure(Root, TEXT("/point"), Error);
			}
			Out.Point = MoveTemp(Parsed);
		}
		if (Out.Kind != TEXT("open") && Out.Kind != TEXT("inclusive") && Out.Kind != TEXT("exclusive"))
			return FieldFailure(Root, TEXT("kind"), TEXT("timeline_animation_bound_invalid"), TEXT("open, inclusive or exclusive"), Error);
		if (Out.Kind == TEXT("open") && Out.Point.IsSet())
			return FieldFailure(Root, TEXT("point"), TEXT("timeline_animation_bound_invalid"), TEXT("point omitted for an open bound"), Error);
		if (Out.Kind != TEXT("open") && !Out.Point.IsSet())
			return FieldFailure(Root, TEXT("point"), TEXT("timeline_animation_bound_invalid"), TEXT("a point for an inclusive or exclusive bound"), Error);
		return true;
	}

	bool ParseRange(
		const TSharedPtr<FJsonObject>& Root,
		FTimelineAnimationRange& Out,
		FUeAgentTimelineAnimationExecutionError& Error)
	{
		ON_SCOPE_EXIT { if (!Error.IsEmpty() && Root) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Root); };
		Out = {};
		TSharedPtr<FJsonObject> Lower;
		TSharedPtr<FJsonObject> Upper;
		if (!RequiredString(Root, TEXT("clock_id"), Out.ClockId, Error)
			|| !RequiredString(Root, TEXT("state"), Out.State, Error)
			|| !RequiredObject(Root, TEXT("lower"), Lower, Error)
			|| !RequiredObject(Root, TEXT("upper"), Upper, Error)) return false;
		if (!ParseBound(Lower, Out.Lower, Error)) return PrefixChildFailure(Root, TEXT("/lower"), Error);
		if (!ParseBound(Upper, Out.Upper, Error)) return PrefixChildFailure(Root, TEXT("/upper"), Error);
		if (Out.State != TEXT("authored")) return FieldFailure(Root, TEXT("state"), TEXT("timeline_animation_range_invalid"), TEXT("authored"), Error);
		if (Out.ClockId.TrimStartAndEnd().IsEmpty()) return FieldFailure(Root, TEXT("clock_id"), TEXT("timeline_animation_range_invalid"), TEXT("a nonempty clock identifier"), Error);
		for (const auto& Bound : {TPair<const TCHAR*, const FTimelineAnimationBound*>(TEXT("lower"), &Out.Lower), {TEXT("upper"), &Out.Upper}})
		{
			if (Bound.Value->Point.IsSet() && Bound.Value->Point->ClockId != Out.ClockId)
			{
				Error = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("timeline_animation_range_invalid"), FString(TEXT("/")) + Bound.Key + TEXT("/point/clock_id"),
					MakeShared<FJsonValueString>(Out.ClockId), MakeShared<FJsonValueString>(Bound.Value->Point->ClockId), TEXT("timeline_decode"), TEXT("The bound point must use the range's clock."));
				Error.ExpectedEvidence = MoveTemp(Error.ExpectedValue);
				Error.ActualEvidence = MoveTemp(Error.ActualValue);
				return false;
			}
		}
		if (!Out.IsValid())
		{
			auto PointValue = [](const FTimelineAnimationPoint& Point) {
				auto Value = MakeShared<FJsonObject>();
				Value->SetStringField(TEXT("tick"), Point.Tick);
				Value->SetStringField(TEXT("rate_numerator"), Point.Rate.Numerator);
				Value->SetStringField(TEXT("rate_denominator"), Point.Rate.Denominator);
				return MakeShared<FJsonValueObject>(Value);
			};
			Error = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("timeline_animation_range_invalid"), TEXT("/upper/point"),
				PointValue(Out.Lower.Point.GetValue()), PointValue(Out.Upper.Point.GetValue()), TEXT("timeline_decode"), TEXT("The upper bound must not precede the lower bound after exact rate conversion; expected_value gives the lower bound."));
			return false;
		}
		return true;
	}

	bool ParseFrame(
		const TSharedPtr<FJsonObject>& Root,
		FTimelineAnimationFrame3d& Out,
		FUeAgentTimelineAnimationExecutionError& Error)
	{
		ON_SCOPE_EXIT { if (!Error.IsEmpty() && Root) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Root); };
		Out = {};
		if (!RequiredString(Root, TEXT("frame_id"), Out.FrameId, Error)
			|| !RequiredString(Root, TEXT("basis_frame_id"), Out.BasisFrameId, Error)
			|| !RequiredNumber(Root, TEXT("dimension"), Out.Dimension, Error)
			|| !RequiredString(Root, TEXT("unit"), Out.Unit, Error)
			|| !RequiredString(Root, TEXT("basis_evidence_id"), Out.BasisEvidenceId, Error)) return false;
		if (Out.Dimension != 3) return FieldFailure(Root, TEXT("dimension"), TEXT("timeline_animation_frame_invalid"), TEXT("3"), Error);
		const TCHAR* EmptyField = Out.FrameId.IsEmpty() ? TEXT("frame_id") : Out.BasisFrameId.IsEmpty() ? TEXT("basis_frame_id")
			: Out.Unit.IsEmpty() ? TEXT("unit") : Out.BasisEvidenceId.IsEmpty() ? TEXT("basis_evidence_id") : nullptr;
		return !EmptyField || FieldFailure(Root, EmptyField, TEXT("timeline_animation_frame_invalid"), TEXT("a nonempty string"), Error);
	}

	bool ParseTransform(
		const TSharedPtr<FJsonObject>& Root,
		FTimelineAnimationTransform3d& Out,
		FUeAgentTimelineAnimationExecutionError& Error)
	{
		ON_SCOPE_EXIT { if (!Error.IsEmpty() && Root) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Root); };
		Out = {};
		TSharedPtr<FJsonObject> Frame;
		if (!RequiredString(Root, TEXT("subject"), Out.Subject, Error)
			|| !RequiredString(Root, TEXT("quantity"), Out.Quantity, Error)
			|| !RequiredObject(Root, TEXT("frame"), Frame, Error)) return false;
		if (!ParseFrame(Frame, Out.Frame, Error)) return PrefixChildFailure(Root, TEXT("/frame"), Error);
		if (!ParseStringArray(Root, TEXT("components"), Out.Components, true, Error)
			|| !ParseStringArray(Root, TEXT("conversion_evidence_ids"), Out.ConversionEvidenceIds, true, Error)) return false;
		if (Out.Subject.IsEmpty()) return FieldFailure(Root, TEXT("subject"), TEXT("timeline_animation_transform_invalid"), TEXT("a nonempty subject"), Error);
		if (Out.Quantity != TEXT("transform")) return FieldFailure(Root, TEXT("quantity"), TEXT("timeline_animation_transform_invalid"), TEXT("transform"), Error);
		if (Out.Components.Num() != 10) return ValueFailure(Root, TEXT("/components"), TEXT("timeline_animation_transform_invalid"), TEXT("exactly 10 components"), MakeShared<FJsonValueNumber>(Out.Components.Num()), Error);
		return !Out.ConversionEvidenceIds.IsEmpty() || FieldFailure(Root, TEXT("conversion_evidence_ids"), TEXT("timeline_animation_transform_invalid"), TEXT("a nonempty array of conversion evidence identifiers"), Error);
	}

	bool ParseTimelineKey(const TSharedPtr<FJsonObject>& Root,
		FTimelineAnimationKey& Out,
		FUeAgentTimelineAnimationExecutionError& Error)
	{
		ON_SCOPE_EXIT { if (!Error.IsEmpty() && Root) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Root); };
		TSharedPtr<FJsonObject> Time;
		Out.SourceRecord = Root;
		Out.bHasValueLexeme = Root->HasField(TEXT("value_lexeme"));
		Out.Value = Root->TryGetField(TEXT("value"));
		if (Out.Value.IsValid() && Out.bHasValueLexeme) return FieldFailure(Root, TEXT("value_lexeme"), TEXT("timeline_animation_key_value_fields_conflict"), TEXT("only one of value and value_lexeme"), Error);
		if (!RequiredString(Root, TEXT("key_ref"), Out.KeyRef, Error)
			|| !RequiredObject(Root, TEXT("time"), Time, Error)
			|| !(ParsePoint(Time, Out.Time, Error) || PrefixChildFailure(Root, TEXT("/time"), Error))
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
			if (!ParseTransform(Transform, Parsed, Error)) return PrefixChildFailure(Root, TEXT("/transform"), Error);
			Out.Transform = MoveTemp(Parsed);
		}
		if (Root->HasField(TEXT("properties")))
		{
			TSharedPtr<FJsonObject> Properties;
			if (!RequiredObject(Root, TEXT("properties"), Properties, Error)) return false;
			Out.Properties = MakeShared<FJsonValueObject>(Properties);
		}
		if (Out.KeyRef.IsEmpty()) return FieldFailure(Root, TEXT("key_ref"), TEXT("timeline_animation_timeline_key_invalid"), TEXT("a nonempty key reference"), Error);
		if (Out.ValueType.IsEmpty()) return FieldFailure(Root, TEXT("value_type"), TEXT("timeline_animation_timeline_key_invalid"), TEXT("a nonempty value type"), Error);
		return Out.ValueType != TEXT("transform") || Out.Transform.IsSet()
			|| FieldFailure(Root, TEXT("transform"), TEXT("timeline_animation_timeline_key_invalid"), TEXT("a transform object when value_type is transform"), Error);
	}

	bool ParseTimelineChannel(const TSharedPtr<FJsonObject>& Root,
		FTimelineAnimationChannel& Out, FUeAgentTimelineAnimationExecutionError& Error)
	{
		ON_SCOPE_EXIT { if (!Error.IsEmpty() && Root) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Root); };
		Out.SourceRecord = Root;
		if (Root->HasField(TEXT("default")))
		{
			TSharedPtr<FJsonObject> Default; bool bSet = false;
			if (!RequiredObject(Root, TEXT("default"), Default, Error)) return false;
			if (!Default->TryGetBoolField(TEXT("is_set"), bSet))
			{
				FieldFailure(Default, TEXT("is_set"), TEXT("timeline_animation_channel_default_invalid"), TEXT("a boolean"), Error);
				return PrefixChildFailure(Root, TEXT("/default"), Error);
			}
			if (Default->Values.Num() != (bSet ? 2 : 1))
				return FieldFailure(Root, TEXT("default"), TEXT("timeline_animation_channel_default_invalid"), TEXT("is_set alone when false, or is_set plus exactly one of value and value_lexeme when true"), Error);
			if (bSet && !Default->HasField(TEXT("value")) && !RequiredString(Default, TEXT("value_lexeme"), Out.DefaultValueLexeme, Error)) return PrefixChildFailure(Root, TEXT("/default"), Error);
			Out.DefaultValue = Default->TryGetField(TEXT("value"));
			Out.DefaultIsSet = bSet;
		}
		if (Root->HasField(TEXT("properties")))
		{
			TSharedPtr<FJsonObject> Properties;
			if (!RequiredObject(Root, TEXT("properties"), Properties, Error)) return false;
			Out.Properties = MakeShared<FJsonValueObject>(Properties);
		}
		return RequiredString(Root, TEXT("channel_ref"), Out.ChannelRef, Error)
			&& RequiredString(Root, TEXT("name"), Out.Name, Error)
			&& RequiredString(Root, TEXT("channel_kind"), Out.ChannelKind, Error)
			&& ParseObjectArray(Root, TEXT("keys"), Out.Keys, true, ParseTimelineKey, Error)
			&& (Out.IsValid() || FieldFailure(Root, Out.ChannelRef.IsEmpty() ? TEXT("channel_ref") : Out.Name.IsEmpty() ? TEXT("name") : TEXT("channel_kind"),
				TEXT("timeline_animation_channel_invalid"), TEXT("a non-empty string"), Error));
	}

	bool ParseTimelineSection(const TSharedPtr<FJsonObject>& Root,
		FTimelineAnimationSection& Out,
		FUeAgentTimelineAnimationExecutionError& Error)
	{
		ON_SCOPE_EXIT { if (!Error.IsEmpty() && Root) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Root); };
		Out.SourceRecord = Root;
		Out.NativeChannels = Root->TryGetField(TEXT("native_channels"));
		if (Out.NativeChannels && Out.NativeChannels->Type != EJson::Array)
			return FieldFailure(Root, TEXT("native_channels"), TEXT("timeline_animation_native_channels_context_invalid"), TEXT("an array"), Error);
		if (Root->HasField(TEXT("channels")) && Root->HasField(TEXT("keys")))
			return FieldFailure(Root, TEXT("keys"), TEXT("timeline_animation_section_fields_conflict"), TEXT("only one of channels and keys"), Error);
		TSharedPtr<FJsonObject> Range;
		bool bProperties = false;
		return RequiredString(Root, TEXT("section_ref"), Out.SectionRef, Error)
			&& OptionalObject(Root, TEXT("properties"), Out.Properties, bProperties, Error)
			&& RequiredString(Root, TEXT("section_kind"), Out.SectionKind, Error)
			&& RequiredObject(Root, TEXT("range"), Range, Error)
			&& (ParseRange(Range, Out.Range, Error) || PrefixChildFailure(Root, TEXT("/range"), Error))
			&& OptionalString(Root, TEXT("source_ref"), Out.SourceRef, Error)
			&& (Root->HasField(TEXT("channels"))
				? ParseObjectArray(Root, TEXT("channels"), Out.Channels, true, ParseTimelineChannel, Error)
				: ParseObjectArray(Root, TEXT("keys"), Out.Keys, true, ParseTimelineKey, Error))
			&& (!Out.SectionRef.IsEmpty() || FieldFailure(Root, TEXT("section_ref"), TEXT("timeline_animation_section_invalid"), TEXT("a nonempty section reference"), Error))
			&& (!Out.SectionKind.IsEmpty() || FieldFailure(Root, TEXT("section_kind"), TEXT("timeline_animation_section_invalid"), TEXT("a nonempty section kind"), Error));
	}

	bool ParseTimelineTrack(const TSharedPtr<FJsonObject>& Root,
		FTimelineAnimationTrack& Out,
		FUeAgentTimelineAnimationExecutionError& Error)
	{
		ON_SCOPE_EXIT { if (!Error.IsEmpty() && Root) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Root); };
		Out.SourceRecord = Root;
		bool bProperties = false;
		return RequiredString(Root, TEXT("track_ref"), Out.TrackRef, Error)
			&& OptionalObject(Root, TEXT("properties"), Out.Properties, bProperties, Error)
			&& OptionalString(Root, TEXT("display_name"), Out.DisplayName, Error)
			&& OptionalString(Root, TEXT("track_kind"), Out.TrackKind, Error)
			&& OptionalString(Root, TEXT("property_path"), Out.PropertyPath, Error)
			&& ParseObjectArray(
				Root, TEXT("sections"), Out.Sections, !Root->HasField(TEXT("name")), ParseTimelineSection, Error)
			&& (!Out.TrackRef.IsEmpty() || FieldFailure(Root, TEXT("track_ref"), TEXT("timeline_animation_track_invalid"), TEXT("a nonempty track reference"), Error))
			&& (!Out.TrackKind.IsEmpty() || Root->HasTypedField<EJson::String>(TEXT("name"))
				|| FieldFailure(Root, TEXT("name"), TEXT("timeline_animation_track_invalid"), TEXT("a string name when track_kind is empty"), Error));
	}

	bool ParseTimelineBinding(const TSharedPtr<FJsonObject>& Root,
		FTimelineAnimationBinding& Out,
		FUeAgentTimelineAnimationExecutionError& Error)
	{
		ON_SCOPE_EXIT { if (!Error.IsEmpty() && Root) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Root); };
		Out.Tags.Reset();
		Out.SourceRecord = Root;
		bool bProperties = false;
		if (!OptionalObject(Root, TEXT("properties"), Out.Properties, bProperties, Error)) return false;
		if (!OptionalObject(Root, TEXT("template_properties"), Out.TemplateProperties, bProperties, Error)) return false;
		if (Root.IsValid() && Root->HasField(TEXT("tags")))
		{
			TArray<FString> Tags;
			if (!ParseStringArray(Root, TEXT("tags"), Tags, true, Error)) return false;
			Out.Tags = MoveTemp(Tags);
		}
		return RequiredString(Root, TEXT("binding_ref"), Out.BindingRef, Error)
			&& RequiredString(Root, TEXT("binding_kind"), Out.BindingKind, Error)
			&& OptionalString(Root, TEXT("object_ref"), Out.ObjectRef, Error)
			&& ParseObjectArray(
				Root, TEXT("tracks"), Out.Tracks, false, ParseTimelineTrack, Error)
			&& (!Out.BindingRef.IsEmpty() || FieldFailure(Root, TEXT("binding_ref"), TEXT("timeline_animation_binding_invalid"), TEXT("a nonempty binding reference"), Error))
			&& (Out.BindingKind == TEXT("possessable") || Out.BindingKind == TEXT("spawnable") || Out.BindingKind == TEXT("widget")
				|| Out.BindingKind == TEXT("slot") || Out.BindingKind == TEXT("root_widget")
				|| FieldFailure(Root, TEXT("binding_kind"), TEXT("timeline_animation_binding_invalid"), TEXT("possessable, spawnable, widget, slot or root_widget"), Error));
	}

	static bool ParseFolderReference(const TSharedPtr<FJsonValue>& Value, FTimelineAnimationItemReference& Out)
	{
		Out = {};
		if (!Value || Value->Type == EJson::Null) return true;
		if (Value->TryGetString(Out.Key)) return true;
		Out.bNew = HjsonWorkspace::TryGetNewKeyedItemReference(*Value, Out.Key);
		return Out.bNew;
	}
	static bool ParseTimelineFolder(const TSharedPtr<FJsonObject>& Root, FTimelineAnimationFolder& Out,
		FUeAgentTimelineAnimationExecutionError& Error)
	{
		ON_SCOPE_EXIT { if (!Error.IsEmpty() && Root) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Root); };
		Out = {};
		Out.SourceRecord = Root;
		if (!Root || !ParseFolderReference(Root->TryGetField(TEXT("folder_ref")), Out.Ref) || Out.Ref.Key.IsEmpty())
			return FieldFailure(Root, TEXT("folder_ref"), TEXT("timeline_animation_folder_reference_invalid"), TEXT("a non-empty existing or new keyed-item reference"), Error);
		if (!ParseFolderReference(Root->TryGetField(TEXT("parent")), Out.Parent))
			return FieldFailure(Root, TEXT("parent"), TEXT("timeline_animation_folder_reference_invalid"), TEXT("an existing or new keyed-item reference, or null"), Error);
		if (!OptionalString(Root, TEXT("name"), Out.Name, Error)) return false;
		if (Out.Name.IsEmpty() && Out.Ref.bNew) Out.Name = Out.Ref.Key;
		auto ReadMembers = [&](const TCHAR* Field, TArray<FTimelineAnimationItemReference>& Members) {
			if (!Root->HasField(Field)) return true;
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Root->TryGetArrayField(Field, Values)) return FieldFailure(Root, Field, TEXT("timeline_animation_folder_members_invalid"), TEXT("an array"), Error);
			for (int32 Index = 0; Index < Values->Num(); ++Index) { FTimelineAnimationItemReference Ref;
				const auto& Value = (*Values)[Index];
				if (!ParseFolderReference(Value, Ref) || Ref.Key.IsEmpty() || Members.Contains(Ref))
					return ValueFailure(Root, FString::Printf(TEXT("/%s/%d"), Field, Index), TEXT("timeline_animation_folder_member_invalid"), TEXT("a non-empty reference not repeated in this folder"), Value, Error);
				Members.Add(MoveTemp(Ref)); }
			return true;
		};
		if (Out.Name.IsEmpty()) return FieldFailure(Root, TEXT("name"), TEXT("timeline_animation_folder_invalid"), TEXT("a non-empty name"), Error);
		return ReadMembers(TEXT("bindings"), Out.Bindings) && ReadMembers(TEXT("root_tracks"), Out.RootTracks);
	}
	bool ParseStructure(const TSharedPtr<FJsonObject>& Root,
		FTimelineAnimationStructure& Out,
		FUeAgentTimelineAnimationExecutionError& Error)
	{
		ON_SCOPE_EXIT { if (!Error.IsEmpty() && Root) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Root); };
		TSharedPtr<FJsonObject> Display;
		Out.Folders.Reset();
		if (Root && Root->HasField(TEXT("folders"))) {
			TArray<FTimelineAnimationFolder> Folders;
			if (!ParseObjectArray(Root, TEXT("folders"), Folders, true, ParseTimelineFolder, Error)) return false;
			Out.Folders = MoveTemp(Folders);
		}
		TSharedPtr<FJsonObject> Tick;
		TSharedPtr<FJsonObject> Range;
		return RequiredObject(Root, TEXT("display_rate"), Display, Error)
			&& (ParseRate(Display, Out.DisplayRate, Error) || PrefixChildFailure(Root, TEXT("/display_rate"), Error))
			&& RequiredObject(Root, TEXT("tick_resolution"), Tick, Error)
			&& (ParseRate(Tick, Out.TickResolution, Error) || PrefixChildFailure(Root, TEXT("/tick_resolution"), Error))
			&& RequiredObject(Root, TEXT("playback_range"), Range, Error)
			&& (ParseRange(Range, Out.PlaybackRange, Error) || PrefixChildFailure(Root, TEXT("/playback_range"), Error))
			&& ParseObjectArray(
				Root, TEXT("bindings"), Out.Bindings, true, ParseTimelineBinding, Error)
			&& ParseObjectArray(
				Root, TEXT("root_tracks"), Out.RootTracks, true, ParseTimelineTrack, Error);
	}

	namespace
	{
		bool TimelineStringRequired(const FString& Value, const FString& Path, FUeAgentTimelineAnimationExecutionError& Error)
		{
			return !Value.IsEmpty() || ValueFailure(nullptr, Path, TEXT("timeline_animation_value_required"),
				TEXT("a non-empty string"), MakeShared<FJsonValueString>(Value), Error);
		}

		template <typename ValueType>
		bool TryTimelineChild(const ValueType& Value, const FString& Path, FUeAgentTimelineAnimationExecutionError& Error)
		{
			return Value.TryValidate(Error) || PrefixChildFailure(nullptr, Path, Error);
		}

		template <typename ValueType>
		bool TryTimelineChildren(const TArray<ValueType>& Values, const FString& Path, FUeAgentTimelineAnimationExecutionError& Error)
		{
			for (int32 Index = 0; Index < Values.Num(); ++Index)
				if (!TryTimelineChild(Values[Index], Path + TEXT("/") + FString::FromInt(Index), Error)) return false;
			return true;
		}

		bool TimelineTemporalCause(const FUeAgentStateSemanticError& Cause, FUeAgentTimelineAnimationExecutionError& Error)
		{
			Error = Cause;
			if (Error.SemanticPointer.IsEmpty() && !Error.PropertyPath.IsEmpty())
				Error.SemanticPointer = Error.PropertyPath.StartsWith(TEXT("/")) ? Error.PropertyPath : TEXT("/") + Error.PropertyPath;
			if (Error.Source.InternalJsonPath.IsEmpty()) Error.Source.InternalJsonPath = Error.SemanticPointer;
			return false;
		}
	}

	bool FTimelineAnimationRate::TryValidate(FUeAgentTimelineAnimationExecutionError& Error) const
	{
		Error = {};
		const FUeAgentTemporalRate Rate(Numerator, Denominator);
		return Rate.IsValid() || TimelineTemporalCause(Rate.GetValidationDiagnostic(), Error);
	}

	bool FTimelineAnimationPoint::TryValidate(FUeAgentTimelineAnimationExecutionError& Error) const
	{
		Error = {};
		if (State != TEXT("authored"))
			return ValueFailure(nullptr, TEXT("/state"), TEXT("timeline_animation_point_invalid"), TEXT("authored"),
				MakeShared<FJsonValueString>(State), Error);
		if (!TryTimelineChild(Rate, TEXT("/rate"), Error)) return false;
		const auto Point = ToTemporalPoint();
		return Point.IsValid() || TimelineTemporalCause(Point.GetValidationDiagnostic(), Error);
	}

	FUeAgentTemporalPoint FTimelineAnimationPoint::ToTemporalPoint() const
	{
		return FUeAgentTemporalPoint(ClockId, Tick, Rate.Numerator, Rate.Denominator, State);
	}

	bool FTimelineAnimationBound::TryValidate(FUeAgentTimelineAnimationExecutionError& Error) const
	{
		Error = {};
		if (Kind == TEXT("open"))
		{
			if (!Point.IsSet()) return true;
			auto Presence = MakeShared<FJsonObject>();
			Presence->SetBoolField(TEXT("present"), true);
			return ValueFailure(nullptr, TEXT("/point"), TEXT("timeline_animation_bound_invalid"),
				TEXT("no point for an open bound"), MakeShared<FJsonValueObject>(Presence), Error);
		}
		if (Kind != TEXT("inclusive") && Kind != TEXT("exclusive"))
			return ValueFailure(nullptr, TEXT("/kind"), TEXT("timeline_animation_bound_invalid"), TEXT("open, inclusive, or exclusive"),
				MakeShared<FJsonValueString>(Kind), Error);
		if (!Point.IsSet())
			return ValueFailure(nullptr, TEXT("/point"), TEXT("timeline_animation_bound_invalid"), TEXT("a point for a closed bound"), nullptr, Error);
		return TryTimelineChild(Point.GetValue(), TEXT("/point"), Error);
	}

	bool FTimelineAnimationRange::TryValidate(FUeAgentTimelineAnimationExecutionError& Error) const
	{
		Error = {};
		if (!TimelineStringRequired(ClockId, TEXT("/clock_id"), Error)) return false;
		if (State != TEXT("authored"))
			return ValueFailure(nullptr, TEXT("/state"), TEXT("timeline_animation_range_invalid"), TEXT("authored"),
				MakeShared<FJsonValueString>(State), Error);
		if (!TryTimelineChild(Lower, TEXT("/lower"), Error) || !TryTimelineChild(Upper, TEXT("/upper"), Error)) return false;
		const auto ToBound = [](const FTimelineAnimationBound& Bound)
		{
			if (Bound.Kind == TEXT("open")) return FUeAgentTemporalBound::Open();
			return Bound.Kind == TEXT("inclusive") ? FUeAgentTemporalBound::Inclusive(Bound.Point->ToTemporalPoint())
				: FUeAgentTemporalBound::Exclusive(Bound.Point->ToTemporalPoint());
		};
		const FUeAgentTemporalRange Range(ClockId, State, ToBound(Lower), ToBound(Upper));
		return Range.IsValid() || TimelineTemporalCause(Range.GetValidationDiagnostic(), Error);
	}

	bool FTimelineAnimationFrame3d::TryValidate(FUeAgentTimelineAnimationExecutionError& Error) const
	{
		Error = {};
		if (!TimelineStringRequired(FrameId, TEXT("/frame_id"), Error) || !TimelineStringRequired(BasisFrameId, TEXT("/basis_frame_id"), Error)) return false;
		if (Dimension != 3)
			return ValueFailure(nullptr, TEXT("/dimension"), TEXT("timeline_animation_frame_invalid"), TEXT("dimension 3"),
				MakeShared<FJsonValueNumber>(Dimension), Error);
		if (!TimelineStringRequired(Unit, TEXT("/unit"), Error)) return false;
		if (BasisEvidenceId.IsEmpty())
			return ValueFailure(nullptr, TEXT("/basis_evidence_id"), TEXT("timeline_animation_frame_invalid"),
				TEXT("non-empty basis evidence"), MakeShared<FJsonValueString>(BasisEvidenceId), Error);
		return true;
	}

	bool FTimelineAnimationTransform3d::TryValidate(FUeAgentTimelineAnimationExecutionError& Error) const
	{
		Error = {};
		if (!TimelineStringRequired(Subject, TEXT("/subject"), Error)) return false;
		if (Quantity != TEXT("transform"))
			return ValueFailure(nullptr, TEXT("/quantity"), TEXT("timeline_animation_transform_invalid"), TEXT("transform"),
				MakeShared<FJsonValueString>(Quantity), Error);
		if (!TryTimelineChild(Frame, TEXT("/frame"), Error)) return false;
		if (Components.Num() != 10)
		{
			auto Shape = MakeShared<FJsonObject>();
			Shape->SetNumberField(TEXT("count"), Components.Num());
			return ValueFailure(nullptr, TEXT("/components"), TEXT("timeline_animation_transform_invalid"), TEXT("10 transform components"),
				MakeShared<FJsonValueObject>(Shape), Error);
		}
		if (ConversionEvidenceIds.IsEmpty())
		{
			auto Shape = MakeShared<FJsonObject>();
			Shape->SetNumberField(TEXT("count"), 0);
			return ValueFailure(nullptr, TEXT("/conversion_evidence_ids"), TEXT("timeline_animation_transform_invalid"),
				TEXT("at least one conversion evidence id"), MakeShared<FJsonValueObject>(Shape), Error);
		}
		return true;
	}

	bool FTimelineAnimationRate::IsValid() const { FUeAgentTimelineAnimationExecutionError Error; return TryValidate(Error); }
	bool FTimelineAnimationPoint::IsValid() const { FUeAgentTimelineAnimationExecutionError Error; return TryValidate(Error); }
	bool FTimelineAnimationBound::IsValid() const { FUeAgentTimelineAnimationExecutionError Error; return TryValidate(Error); }
	bool FTimelineAnimationRange::IsValid() const { FUeAgentTimelineAnimationExecutionError Error; return TryValidate(Error); }
	bool FTimelineAnimationFrame3d::IsValid() const { FUeAgentTimelineAnimationExecutionError Error; return TryValidate(Error); }
	bool FTimelineAnimationTransform3d::IsValid() const { FUeAgentTimelineAnimationExecutionError Error; return TryValidate(Error); }

	bool FTimelineAnimationTransform3d::TryToNativeTransform(
		const FString& NativeFrameId,
		FTransform& OutTransform,
		FUeAgentTimelineAnimationExecutionError& OutError) const
	{
		OutTransform = FTransform::Identity;
		if (!IsValid() || Frame.FrameId != NativeFrameId
			|| Frame.Unit != TEXT("centimeter"))
		{
			const TCHAR* Path = nullptr;
			FString Expected, Actual;
			if (Subject.IsEmpty()) { Path = TEXT("/subject"); Expected = TEXT("nonempty subject"); Actual = Subject; }
			else if (Quantity != TEXT("transform")) { Path = TEXT("/quantity"); Expected = TEXT("transform"); Actual = Quantity; }
			else if (Frame.FrameId.IsEmpty() || Frame.FrameId != NativeFrameId) { Path = TEXT("/frame/frame_id"); Expected = NativeFrameId; Actual = Frame.FrameId; }
			else if (Frame.BasisFrameId.IsEmpty()) { Path = TEXT("/frame/basis_frame_id"); Expected = TEXT("nonempty basis frame"); Actual = Frame.BasisFrameId; }
			else if (Frame.Dimension != 3) { Path = TEXT("/frame/dimension"); Expected = TEXT("3"); Actual = FString::FromInt(Frame.Dimension); }
			else if (Frame.Unit != TEXT("centimeter")) { Path = TEXT("/frame/unit"); Expected = TEXT("centimeter"); Actual = Frame.Unit; }
			else if (Frame.BasisEvidenceId.IsEmpty()) { Path = TEXT("/frame/basis_evidence_id"); Expected = TEXT("nonempty basis evidence"); Actual = Frame.BasisEvidenceId; }
			else if (Components.Num() != 10) { Path = TEXT("/components"); Expected = TEXT("10 components"); Actual = FString::FromInt(Components.Num()); }
			else { Path = TEXT("/conversion_evidence_ids"); Expected = TEXT("at least one conversion evidence identifier"); Actual = TEXT("empty array"); }
			OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("timeline_animation_space_conversion_unavailable"), Path,
				MakeShared<FJsonValueString>(Expected), MakeShared<FJsonValueString>(Actual), TEXT("native_execution"), TEXT("The transform field does not meet the native coordinate conversion requirement."));
			if (FCString::Strcmp(Path, TEXT("/frame/frame_id")) == 0) { OutError.ExpectedEvidence = MoveTemp(OutError.ExpectedValue); OutError.ActualEvidence = MoveTemp(OutError.ActualValue); }
			return false;
		}
		TArray<double> Values;
		for (int32 Index = 0; Index < Components.Num(); ++Index)
		{
			const FString& Component = Components[Index];
			double Value = 0.0;
			if (!TryParseFiniteDecimal(Component, Value))
			{
				OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("timeline_animation_transform_component_invalid"), TEXT("/components/") + FString::FromInt(Index), MakeShared<FJsonValueString>(TEXT("finite decimal number")), MakeShared<FJsonValueString>(Component), TEXT("native_execution"), TEXT("The transform component cannot be parsed as a finite decimal number."));
				return false;
			}
			Values.Add(Value);
		}
		FQuat Rotation(Values[3], Values[4], Values[5], Values[6]);
		UE::UAI::Contract::FDiagnostic RotationError;
		if (!UeAgentPropertyValueCodec::TryNormalizeQuaternion(Rotation, RotationError)) {
			OutError = MoveTemp(RotationError);
			OutError.SemanticPointer = TEXT("/components");
			return false;
		}
		OutTransform = FTransform(
			Rotation,
			FVector(Values[0], Values[1], Values[2]),
			FVector(Values[7], Values[8], Values[9]));
		if (OutTransform.ContainsNaN()) {
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("timeline_animation_native_transform_invalid"), TEXT("Constructing the native transform produced a nonfinite component."), TEXT("native_execution"));
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = TEXT("transform");
			return false;
		}
		return true;
	}

	bool FTimelineAnimationKey::TryValidate(FUeAgentTimelineAnimationExecutionError& Error) const
	{
		Error = {};
		if (!TimelineStringRequired(KeyRef, TEXT("/key_ref"), Error) || !TryTimelineChild(Time, TEXT("/time"), Error)
			|| !TimelineStringRequired(ValueType, TEXT("/value_type"), Error)) return false;
		if (ValueType == TEXT("transform"))
		{
			if (!Transform.IsSet()) return ValueFailure(nullptr, TEXT("/transform"), TEXT("timeline_animation_key_invalid"), TEXT("a transform value"), nullptr, Error);
			return TryTimelineChild(Transform.GetValue(), TEXT("/transform"), Error);
		}
		return true;
	}

	bool FTimelineAnimationChannel::TryValidate(FUeAgentTimelineAnimationExecutionError& Error) const
	{
		Error = {};
		return TimelineStringRequired(ChannelRef, TEXT("/channel_ref"), Error)
			&& TimelineStringRequired(Name, TEXT("/name"), Error)
			&& TimelineStringRequired(ChannelKind, TEXT("/channel_kind"), Error)
			&& TryTimelineChildren(Keys, TEXT("/keys"), Error);
	}

	bool FTimelineAnimationSection::TryValidate(FUeAgentTimelineAnimationExecutionError& Error) const
	{
		Error = {};
		return TimelineStringRequired(SectionRef, TEXT("/section_ref"), Error)
			&& TimelineStringRequired(SectionKind, TEXT("/section_kind"), Error)
			&& TryTimelineChild(Range, TEXT("/range"), Error)
			&& TryTimelineChildren(Keys, TEXT("/keys"), Error)
			&& TryTimelineChildren(Channels, TEXT("/channels"), Error);
	}

	bool FTimelineAnimationTrack::TryValidate(FUeAgentTimelineAnimationExecutionError& Error) const
	{
		Error = {};
		if (!TimelineStringRequired(TrackRef, TEXT("/track_ref"), Error)) return false;
		if (TrackKind.IsEmpty() && !(SourceRecord && SourceRecord->HasTypedField<EJson::String>(TEXT("name"))))
			return ValueFailure(nullptr, TEXT("/name"), TEXT("timeline_animation_track_invalid"), TEXT("a string source name when track_kind is empty"),
				MakeShared<FJsonValueString>(TrackKind), Error);
		return TryTimelineChildren(Sections, TEXT("/sections"), Error);
	}

	bool FTimelineAnimationBinding::TryValidate(FUeAgentTimelineAnimationExecutionError& Error) const
	{
		Error = {};
		if (!TimelineStringRequired(BindingRef, TEXT("/binding_ref"), Error)) return false;
		if (BindingKind != TEXT("possessable") && BindingKind != TEXT("spawnable") && BindingKind != TEXT("widget")
			&& BindingKind != TEXT("slot") && BindingKind != TEXT("root_widget"))
			return ValueFailure(nullptr, TEXT("/binding_kind"), TEXT("timeline_animation_binding_invalid"),
				TEXT("possessable, spawnable, widget, slot, or root_widget"), MakeShared<FJsonValueString>(BindingKind), Error);
		return TryTimelineChildren(Tracks, TEXT("/tracks"), Error);
	}

	bool FTimelineAnimationStructure::TryValidate(FUeAgentTimelineAnimationExecutionError& Error) const
	{
		Error = {};
		return TryTimelineChild(DisplayRate, TEXT("/display_rate"), Error)
			&& TryTimelineChild(TickResolution, TEXT("/tick_resolution"), Error)
			&& TryTimelineChild(PlaybackRange, TEXT("/playback_range"), Error)
			&& TryTimelineChildren(Bindings, TEXT("/bindings"), Error)
			&& TryTimelineChildren(RootTracks, TEXT("/root_tracks"), Error);
	}

	bool FTimelineAnimationKey::IsValid() const { FUeAgentTimelineAnimationExecutionError Error; return TryValidate(Error); }
	bool FTimelineAnimationChannel::IsValid() const { FUeAgentTimelineAnimationExecutionError Error; return TryValidate(Error); }
	bool FTimelineAnimationSection::IsValid() const { FUeAgentTimelineAnimationExecutionError Error; return TryValidate(Error); }
	bool FTimelineAnimationTrack::IsValid() const { FUeAgentTimelineAnimationExecutionError Error; return TryValidate(Error); }
	bool FTimelineAnimationBinding::IsValid() const { FUeAgentTimelineAnimationExecutionError Error; return TryValidate(Error); }
	bool FTimelineAnimationStructure::IsValid() const { FUeAgentTimelineAnimationExecutionError Error; return TryValidate(Error); }
}
