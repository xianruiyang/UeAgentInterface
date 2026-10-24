#include "Utilities/AuthoringCore/UeAgentMovieSceneOriginalIntent.h"

#include "Utilities/AuthoringCore/UeAgentMovieSceneAuthoring.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Utilities/AuthoringCore/UeAgentTimelineValueJson.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "IMovieSceneRetimingInterface.h"
#include "MovieSceneTimeHelpers.h"

namespace UE::UAI::Authoring::Timeline
{
	namespace
	{
		bool ReadRate(const TSharedPtr<FJsonObject>& Scene, FFrameRate& Rate, UE::UAI::Contract::FDiagnostic& Error)
		{
			const TSharedPtr<FJsonObject>* Value = nullptr;
			FTimelineAnimationRate Parsed;
			FUeAgentTimelineAnimationExecutionError Failure;
			if (!Scene->TryGetObjectField(TEXT("tick_resolution"), Value))
				return Json::FieldFailure(Scene, TEXT("tick_resolution"), TEXT("movie_scene_intent_rate_missing"), TEXT("a frame rate object"), Error);
			if (!ParseRate(*Value, Parsed, Failure) || !TryRate(Parsed, Rate, Failure))
			{
				Error = MoveTemp(Failure);
				if (Error.Source.SourceFile.IsEmpty())
				{
					UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Error, TEXT("/tick_resolution"));
					UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Scene);
				}
				return false;
			}
			return true;
		}

		TSharedRef<FJsonObject> RateValue(const FFrameRate& Rate)
		{
			auto Value = MakeShared<FJsonObject>();
			Value->SetStringField(TEXT("numerator"), FString::FromInt(Rate.Numerator));
			Value->SetStringField(TEXT("denominator"), FString::FromInt(Rate.Denominator));
			return Value;
		}

		bool ProjectPoint(const TSharedPtr<FJsonObject>& Point,
			const UE::MovieScene::IRetimingInterface& Retimer, const FFrameRate& Destination, UE::UAI::Contract::FDiagnostic& Error)
		{
			FString Text;
			int64 Tick = 0;
			if (!Point->TryGetStringField(TEXT("tick"), Text))
				return Json::FieldFailure(Point, TEXT("tick"), TEXT("movie_scene_intent_tick_invalid"), TEXT("a decimal integer string"), Error);
			if (!StateSemantic::TryParseInt64Decimal(Text, Tick))
				return Json::FieldFailure(Point, TEXT("tick"), TEXT("movie_scene_intent_tick_invalid"), TEXT("a decimal integer representable as int64"), Error);
			if (Tick < MIN_int32 || Tick > MAX_int32)
				return Json::FieldFailure(Point, TEXT("tick"), TEXT("movie_scene_intent_tick_invalid"), TEXT("an integer in [-2147483648, 2147483647] for a movie-scene frame number"), Error);
			Point->SetStringField(TEXT("tick"), FString::FromInt(Retimer.RemapTime(FFrameNumber(int32(Tick))).Value));
			Point->SetObjectField(TEXT("rate"), RateValue(Destination));
			return true;
		}

		bool ProjectRange(const TSharedPtr<FJsonObject>& Range, const FFrameRate& Source,
			const FFrameRate& Destination, bool bSection, TRange<FFrameNumber>& Result, UE::UAI::Contract::FDiagnostic& Error)
		{
			FTimelineAnimationRange Parsed;
			FUeAgentTimelineAnimationExecutionError Failure;
			if (!ParseRange(Range, Parsed, Failure)
				|| !TrySectionRange(Parsed, Parsed.ClockId, Source, Result, Failure))
			{ Error = Failure; return false; }
			if (Source == Destination) return true;
			const UE::MovieScene::FFrameRateRetiming Retimer(Source, Destination);
			for (const TCHAR* Name : {TEXT("lower"), TEXT("upper")})
			{
				auto Bound = Range->GetObjectField(Name);
				if (Bound->GetStringField(TEXT("kind")) == TEXT("open")) continue;
				auto Point = Bound->GetObjectField(TEXT("point"));
				if (bSection)
				{
					// Section migration uses the discrete start/end, unlike playback
					// ranges, whose authored inclusive/exclusive kind is retained.
					const bool bLower = FCString::Strcmp(Name, TEXT("lower")) == 0;
					const auto Tick = bLower ? UE::MovieScene::DiscreteInclusiveLower(Result)
						: UE::MovieScene::DiscreteExclusiveUpper(Result);
					Point->SetStringField(TEXT("tick"), FString::FromInt(Tick.Value));
					Bound->SetStringField(TEXT("kind"), bLower ? TEXT("inclusive") : TEXT("exclusive"));
				}
				if (!ProjectPoint(Point, Retimer, Destination, Error))
					return Json::PrefixChildFailure(Range, FString(TEXT("/")) + Name + TEXT("/point"), Error);
			}
			if (!ParseRange(Range, Parsed, Failure)
				|| !TrySectionRange(Parsed, Parsed.ClockId, Destination, Result, Failure))
			{ Error = Failure; return false; }
			return true;
		}

		void ProjectRoll(const TSharedPtr<FJsonObject>& Section, const UE::MovieScene::IRetimingInterface& Retimer)
		{
			const TSharedPtr<FJsonObject>* Properties = nullptr;
			if (!Section->TryGetObjectField(TEXT("properties"), Properties)) return;
			for (const auto& Category : (*Properties)->Values)
			{
				if (Category.Value->Type != EJson::Object) continue;
				for (const TCHAR* Name : {TEXT("PreRollFrames"), TEXT("PostRollFrames")})
				{
					const TSharedPtr<FJsonObject> *Descriptor = nullptr, *Value = nullptr, *Fields = nullptr;
					double Number = 0;
					if (Category.Value->AsObject()->TryGetObjectField(Name, Descriptor)
						&& (*Descriptor)->TryGetObjectField(TEXT("value"), Value)
						&& (*Value)->TryGetObjectField(TEXT("fields"), Fields)
						&& (*Fields)->TryGetNumberField(TEXT("Value"), Number) && Number > 0)
						(*Fields)->SetNumberField(TEXT("Value"), Retimer.RemapTime(FFrameNumber(int32(Number))).Value);
				}
			}
		}

		void ProjectTangents(const TSharedPtr<FJsonObject>& Key, double Scale)
		{
			const TSharedPtr<FJsonObject> *Properties = nullptr, *Fields = nullptr, *Tangent = nullptr, *TangentFields = nullptr;
			if (!Key->TryGetObjectField(TEXT("properties"), Properties)
				|| !(*Properties)->TryGetObjectField(TEXT("fields"), Fields)
				|| !(*Fields)->TryGetObjectField(TEXT("Tangent"), Tangent)
				|| !(*Tangent)->TryGetObjectField(TEXT("fields"), TangentFields)) return;
			for (const TCHAR* Name : {TEXT("ArriveTangent"), TEXT("LeaveTangent")})
			{
				const TSharedPtr<FJsonObject>* Value = nullptr;
				double Number = 0;
				if ((*TangentFields)->TryGetObjectField(Name, Value) && (*Value)->TryGetNumberField(TEXT("literal"), Number))
					(*Value)->SetNumberField(TEXT("literal"), float(float(Number) * Scale));
			}
		}
	}

	bool ProjectMovieSceneOriginalIntent(TSharedPtr<FJsonValue>& Before,
		TSharedPtr<FJsonValue>& Desired, TSharedPtr<FJsonValue>& Actual,
		TMap<FString, FString>& SourcePaths, UE::UAI::Contract::FDiagnostic& Error)
	{
		if (!Before || !Desired || Before->Type != EJson::Object || Desired->Type != EJson::Object
			|| !Before->AsObject()->HasField(TEXT("tick_resolution"))) return true;
		FFrameRate Source, Destination;
		if (!ReadRate(Before->AsObject(), Source, Error)) { Error.SourceKind = TEXT("native_result"); return false; }
		if (!ReadRate(Desired->AsObject(), Destination, Error)) return false;
		Desired = UeAgentAuthoringState::CloneCanonicalValue(Desired);
		auto Scene = Desired->AsObject();
		const UE::MovieScene::FFrameRateRetiming Retimer(Source, Destination);
		const bool bRetime = Source != Destination;
		const TSharedPtr<FJsonObject>* Playback = nullptr;
		if (Scene->TryGetObjectField(TEXT("playback_range"), Playback))
		{
			TRange<FFrameNumber> Range;
			if (!ProjectRange(*Playback, Source, Destination, false, Range, Error)) return Json::PrefixChildFailure(Scene, TEXT("/playback_range"), Error);
		}
		auto ProjectTracks = [&](const TArray<TSharedPtr<FJsonValue>>& Tracks, const FString& Prefix)
		{
			for (int32 TrackIndex = 0; TrackIndex < Tracks.Num(); ++TrackIndex)
			{
				auto Track = Tracks[TrackIndex]->AsObject();
				const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
				if (!Track->TryGetArrayField(TEXT("sections"), Sections)) continue;
				TArray<TSharedPtr<FJsonValue>> Kept;
				for (int32 SourceIndex = 0; SourceIndex < Sections->Num(); ++SourceIndex)
				{
					auto Section = (*Sections)[SourceIndex]->AsObject();
					const TSharedPtr<FJsonObject>* RangeValue = nullptr;
					if (!Section->TryGetObjectField(TEXT("range"), RangeValue)) continue;
					TRange<FFrameNumber> Range;
					const FString OriginalSectionPath = Prefix + TEXT("/") + FString::FromInt(TrackIndex) + TEXT("/sections/") + FString::FromInt(SourceIndex);
					if (!ProjectRange(*RangeValue, Source, Destination, true, Range, Error)) return Json::PrefixChildFailure(Scene, OriginalSectionPath + TEXT("/range"), Error);
					if (Range.IsEmpty()) continue;
					const FString SectionPath = Prefix + TEXT("/") + FString::FromInt(TrackIndex)
						+ TEXT("/sections/") + FString::FromInt(Kept.Num());
					SourcePaths.Add(SectionPath, Prefix + TEXT("/") + FString::FromInt(TrackIndex)
						+ TEXT("/sections/") + FString::FromInt(SourceIndex));
					Section->SetStringField(TEXT("section_ref"), Track->GetStringField(TEXT("track_ref"))
						+ TEXT(":section:") + FString::FromInt(Kept.Num()));
					if (bRetime) ProjectRoll(Section, Retimer);
					const TArray<TSharedPtr<FJsonValue>>* Channels = nullptr;
					if (Section->TryGetArrayField(TEXT("channels"), Channels))
						for (int32 ChannelIndex = 0; ChannelIndex < Channels->Num(); ++ChannelIndex)
						{
							auto Channel = (*Channels)[ChannelIndex]->AsObject();
							const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
							if (!Channel->TryGetArrayField(TEXT("keys"), Keys)) continue;
							TArray<TPair<int32, TSharedPtr<FJsonValue>>> Ordered;
							for (int32 KeyIndex = 0; KeyIndex < Keys->Num(); ++KeyIndex)
							{
								auto Key = (*Keys)[KeyIndex]->AsObject();
								if (bRetime && (!ProjectPoint(Key->GetObjectField(TEXT("time")), Retimer, Destination, Error)))
									return Json::PrefixChildFailure(Scene, OriginalSectionPath + TEXT("/channels/") + FString::FromInt(ChannelIndex) + TEXT("/keys/") + FString::FromInt(KeyIndex) + TEXT("/time"), Error);
								if (bRetime) ProjectTangents(Key, Retimer.GetScale());
								Ordered.Emplace(KeyIndex, (*Keys)[KeyIndex]);
							}
							Ordered.StableSort([](const auto& A, const auto& B) {
								return FCString::Atoi64(*A.Value->AsObject()->GetObjectField(TEXT("time"))->GetStringField(TEXT("tick")))
									< FCString::Atoi64(*B.Value->AsObject()->GetObjectField(TEXT("time"))->GetStringField(TEXT("tick"))); });
							TArray<TSharedPtr<FJsonValue>> Sorted;
							for (const auto& Key : Ordered)
							{
								const FString KeyPath = SectionPath + TEXT("/channels/") + FString::FromInt(ChannelIndex)
									+ TEXT("/keys/") + FString::FromInt(Sorted.Num());
								SourcePaths.Add(KeyPath, Prefix + TEXT("/") + FString::FromInt(TrackIndex) + TEXT("/sections/")
									+ FString::FromInt(SourceIndex) + TEXT("/channels/") + FString::FromInt(ChannelIndex)
									+ TEXT("/keys/") + FString::FromInt(Key.Key));
								Key.Value->AsObject()->SetStringField(TEXT("key_ref"), Channel->GetStringField(TEXT("channel_ref"))
									+ TEXT(":key:") + FString::FromInt(Sorted.Num()));
								Sorted.Add(Key.Value);
							}
							Channel->SetArrayField(TEXT("keys"), MoveTemp(Sorted));
						}
					Kept.Add((*Sections)[SourceIndex]);
				}
				Track->SetArrayField(TEXT("sections"), MoveTemp(Kept));
			}
			return true;
		};
		const TArray<TSharedPtr<FJsonValue>>* Tracks = nullptr;
		if (Scene->TryGetArrayField(TEXT("root_tracks"), Tracks) && !ProjectTracks(*Tracks, TEXT("/root_tracks"))) return false;
		const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
		if (Scene->TryGetArrayField(TEXT("bindings"), Bindings))
			for (int32 Index = 0; Index < Bindings->Num(); ++Index)
				if ((*Bindings)[Index]->AsObject()->TryGetArrayField(TEXT("tracks"), Tracks)
					&& !ProjectTracks(*Tracks, TEXT("/bindings/") + FString::FromInt(Index) + TEXT("/tracks"))) return false;
		return true;
	}
}
