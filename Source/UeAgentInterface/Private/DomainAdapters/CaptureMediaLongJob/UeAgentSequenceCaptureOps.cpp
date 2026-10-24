// Copyright Epic Games, Inc. All Rights Reserved.

#include "DomainAdapters/CaptureMediaLongJob/UeAgentSequenceCaptureOps.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureMediaDiagnostics.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "LevelEditorViewport.h"
#include "LevelSequence.h"
#include "Dom/JsonObject.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringAssetIdentity.h"

namespace UE::UAI::DomainAdapters::CaptureMediaLongJob
{
ULevelSequence* LoadLevelSequence(const FString& InPath)
{
	const FString ObjectPath = UeAgentAuthoringAssetIdentity::ToObjectPath(InPath);
	return ObjectPath.IsEmpty() ? nullptr : LoadObject<ULevelSequence>(nullptr, *ObjectPath);
}

bool TryGetSequenceNumberAnyField(const TSharedPtr<FJsonObject>& Obj, const TArray<FString>& Keys, double& OutValue, FString* OutField)
{
	if (OutField) OutField->Reset();
	if (!Obj.IsValid()) return false;
	for (const FString& Key : Keys)
	{
		if (Obj->TryGetNumberField(Key, OutValue))
		{
			if (OutField) *OutField = Key;
			return true;
		}
	}
	return false;
}

bool AddLevelSequenceFrameRequest(UMovieScene* MovieScene, const double RelativeTimeSeconds, const int32 FrameIndex, TArray<FLevelSequenceFrameRequest>& OutFrames, UE::UAI::Contract::FDiagnostic& OutError, const FString& InputPointer)
{
	using namespace CaptureDiagnostics;
	if (!MovieScene)
	{
		OutError = Native(TEXT("movie_scene_not_found"), TEXT("The level sequence has no movie scene."),
			TEXT("sequence_capture"), TEXT("movie_scene_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
		return false;
	}

	const TRange<FFrameNumber> PlaybackRange = MovieScene->GetPlaybackRange();
	const FFrameNumber StartTick = PlaybackRange.GetLowerBoundValue();
	const FFrameNumber EndExclusiveTick = PlaybackRange.GetUpperBoundValue();
	if (EndExclusiveTick.Value <= StartTick.Value)
	{
		OutError = Native(TEXT("invalid_sequence_playback_range"), TEXT("The sequence playback end must be after its start."),
			TEXT("sequence_capture"), TEXT("playback_range_ticks"),
			Facts({ { TEXT("start"), Number(StartTick.Value) }, { TEXT("end_exclusive"), Number(EndExclusiveTick.Value) } }),
			MakeShared<FJsonValueString>(TEXT("end_exclusive > start")));
		OutError.TargetPath = MovieScene->GetPathName();
		return false;
	}

	const FFrameRate TickResolution = MovieScene->GetTickResolution();
	const double AbsoluteTimeSeconds = TickResolution.AsSeconds(StartTick) + RelativeTimeSeconds;
	const FFrameNumber EvalTick = TickResolution.AsFrameTime(AbsoluteTimeSeconds).RoundToFrame();
	if (EvalTick.Value < StartTick.Value || EvalTick.Value >= EndExclusiveTick.Value)
	{
		OutError = Input(TEXT("preview_frame_range_exceeds_sequence"), TEXT("The requested sample evaluates outside the sequence playback range."),
			TEXT("sequence_capture"), InputPointer,
			Facts({ { TEXT("sample_index"), Number(OutFrames.Num()) }, { TEXT("frame_index"), Number(FrameIndex) },
				{ TEXT("relative_time_seconds"), Number(RelativeTimeSeconds) }, { TEXT("evaluation_tick"), Number(EvalTick.Value) } }),
			Facts({ { TEXT("minimum_tick"), Number(StartTick.Value) }, { TEXT("maximum_tick_exclusive"), Number(EndExclusiveTick.Value) } }));
		OutError.TargetPath = MovieScene->GetPathName();
		return false;
	}

	FLevelSequenceFrameRequest Request;
	Request.FrameIndex = FrameIndex;
	Request.TimeSeconds = RelativeTimeSeconds;
	Request.EvaluationFrame = EvalTick;
	OutFrames.Add(Request);
	return true;
}

bool ResolveLevelSequenceFrameSequence(ULevelSequence* Sequence, const TSharedPtr<FJsonObject>& Params, TArray<FLevelSequenceFrameRequest>& OutFrames, UE::UAI::Contract::FDiagnostic& OutError)
{
	using namespace CaptureDiagnostics;
	OutFrames.Reset();
	if (!Sequence || !Params.IsValid())
	{
		OutError = !Sequence
			? Native(TEXT("level_sequence_not_found"), TEXT("The level sequence is unavailable."), TEXT("sequence_capture"),
				TEXT("sequence_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true))
			: Input(TEXT("level_sequence_not_found"), TEXT("Sequence capture parameters must be an object."),
				TEXT("sequence_capture"), FString(), nullptr, MakeShared<FJsonValueString>(TEXT("object")));
		return false;
	}

	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (!MovieScene)
	{
		OutError = Native(TEXT("movie_scene_not_found"), TEXT("The level sequence has no movie scene."),
			TEXT("sequence_capture"), TEXT("movie_scene_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
		OutError.TargetPath = Sequence->GetPathName();
		return false;
	}

	const FFrameRate DisplayRate = MovieScene->GetDisplayRate();
	const double FramesPerSecond = DisplayRate.IsValid() && DisplayRate.AsDecimal() > KINDA_SMALL_NUMBER ? DisplayRate.AsDecimal() : 30.0;
	const double SecondsPerFrame = 1.0 / FramesPerSecond;

	const TArray<TSharedPtr<FJsonValue>>* ExplicitFrames = nullptr;
	if (Params->TryGetArrayField(TEXT("frames"), ExplicitFrames) && ExplicitFrames)
	{
		if (ExplicitFrames->Num() < 1)
		{
			OutError = Input(TEXT("invalid_frame_count"), TEXT("At least one explicit sequence frame is required."),
				TEXT("sequence_capture"), TEXT("/frames"), Number(ExplicitFrames->Num()), MakeShared<FJsonValueString>(TEXT("at least one frame")));
			return false;
		}
		for (int32 Index = 0; Index < ExplicitFrames->Num(); ++Index)
		{
			double FrameValue = 0.0;
			double TimeValue = 0.0;
			bool bUseTime = false;
			FString FrameField;
			FString Pointer = TEXT("/frames/") + ::LexToString(Index);
			if ((*ExplicitFrames)[Index]->Type == EJson::Number)
			{
				FrameValue = (*ExplicitFrames)[Index]->AsNumber();
			}
			else if ((*ExplicitFrames)[Index]->Type == EJson::Object)
			{
				const TSharedPtr<FJsonObject> FrameObject = (*ExplicitFrames)[Index]->AsObject();
				bUseTime = TryGetSequenceNumberAnyField(FrameObject, { TEXT("time_seconds"), TEXT("seconds") }, TimeValue, &FrameField);
				if (!bUseTime && !TryGetSequenceNumberAnyField(FrameObject, { TEXT("frame_index"), TEXT("frame"), TEXT("display_frame") }, FrameValue, &FrameField))
				{
					OutError = Input(TEXT("invalid_frame_entry"), TEXT("The frame object has no numeric time or frame field."),
						TEXT("sequence_capture"), Pointer, (*ExplicitFrames)[Index],
						MakeShared<FJsonValueString>(TEXT("numeric time_seconds/seconds or frame_index/frame/display_frame")));
					return false;
				}
				Pointer += TEXT("/") + FrameField;
			}
			else
			{
				OutError = Input(TEXT("invalid_frame_entry"), TEXT("Each sequence frame must be a number or a frame object."),
					TEXT("sequence_capture"), Pointer, (*ExplicitFrames)[Index],
					MakeShared<FJsonValueString>(TEXT("number or object")));
				return false;
			}

			const int32 FrameIndex = bUseTime ? FMath::RoundToInt(TimeValue / SecondsPerFrame) : FMath::RoundToInt(FrameValue);
			const double RelativeTimeSeconds = bUseTime ? TimeValue : static_cast<double>(FrameIndex) * SecondsPerFrame;
			if (RelativeTimeSeconds < -KINDA_SMALL_NUMBER || !AddLevelSequenceFrameRequest(MovieScene, RelativeTimeSeconds, FrameIndex, OutFrames, OutError, Pointer))
			{
				if (OutError.IsEmpty())
					OutError = Input(TEXT("invalid_time_seconds"), TEXT("The requested frame evaluates to a negative sequence time."),
						TEXT("sequence_capture"), Pointer,
						Facts({ { TEXT("input_value"), Number(bUseTime ? TimeValue : FrameValue) }, { TEXT("relative_time_seconds"), Number(RelativeTimeSeconds) } }),
						Facts({ { TEXT("minimum_time_seconds"), Number(-KINDA_SMALL_NUMBER) } }));
				return false;
			}
		}
		return true;
	}

	double CountValue = 1.0;
	FString CountField;
	TryGetSequenceNumberAnyField(Params, { TEXT("count"), TEXT("frame_count"), TEXT("sample_count") }, CountValue, &CountField);
	const int32 RequestedCount = FMath::RoundToInt(CountValue);
	if (RequestedCount < 1)
	{
		OutError = Input(TEXT("invalid_frame_count"), TEXT("The rounded sequence sample count must be at least one."),
			TEXT("sequence_capture"), TEXT("/") + CountField,
			Facts({ { TEXT("input_value"), Number(CountValue) }, { TEXT("rounded_count"), Number(RequestedCount) } }),
			MakeShared<FJsonValueString>(TEXT("rounded count >= 1")));
		return false;
	}

	double StartTimeSeconds = 0.0;
	FString StartTimeField, IntervalField, StartFrameField, FrameIntervalField;
	const bool bHasStartTime = TryGetSequenceNumberAnyField(Params, { TEXT("start_time_seconds"), TEXT("first_time_seconds"), TEXT("time_seconds") }, StartTimeSeconds, &StartTimeField);
	double IntervalSeconds = SecondsPerFrame;
	const bool bHasIntervalSeconds = TryGetSequenceNumberAnyField(Params, { TEXT("interval_seconds"), TEXT("time_interval_seconds") }, IntervalSeconds, &IntervalField);
	double StartFrameValue = 0.0;
	const bool bHasStartFrame = TryGetSequenceNumberAnyField(Params, { TEXT("start_frame_index"), TEXT("first_frame_index"), TEXT("frame_index"), TEXT("frame"), TEXT("display_frame") }, StartFrameValue, &StartFrameField);
	double FrameIntervalValue = 1.0;
	const bool bHasFrameInterval = TryGetSequenceNumberAnyField(Params, { TEXT("frame_interval"), TEXT("frame_stride") }, FrameIntervalValue, &FrameIntervalField);

	if (bHasStartTime || bHasIntervalSeconds)
	{
		if (!bHasIntervalSeconds && bHasFrameInterval) IntervalSeconds = FrameIntervalValue * SecondsPerFrame;
		if (IntervalSeconds < 0.0)
		{
			OutError = Input(TEXT("invalid_interval_seconds"), TEXT("The sequence sampling interval must not be negative."),
				TEXT("sequence_capture"), TEXT("/") + (bHasIntervalSeconds ? IntervalField : FrameIntervalField),
				Facts({ { TEXT("input_value"), Number(bHasIntervalSeconds ? IntervalSeconds : FrameIntervalValue) }, { TEXT("interval_seconds"), Number(IntervalSeconds) } }),
				MakeShared<FJsonValueString>(TEXT("interval_seconds >= 0")));
			return false;
		}
		for (int32 Index = 0; Index < RequestedCount; ++Index)
		{
			const double TimeSeconds = StartTimeSeconds + IntervalSeconds * static_cast<double>(Index);
			if (TimeSeconds < -KINDA_SMALL_NUMBER)
			{
				OutError = Input(TEXT("invalid_time_seconds"), TEXT("The sequence sample evaluates to a negative time."),
					TEXT("sequence_capture"), StartTimeField.IsEmpty() ? FString() : TEXT("/") + StartTimeField,
					Facts({ { TEXT("sample_index"), Number(Index) }, { TEXT("time_seconds"), Number(TimeSeconds) } }),
					Facts({ { TEXT("minimum_time_seconds"), Number(-KINDA_SMALL_NUMBER) } }));
				return false;
			}
			// Generated samples depend on start, interval and count; do not attribute them to one arbitrary input field.
			if (!AddLevelSequenceFrameRequest(MovieScene, TimeSeconds, FMath::RoundToInt(TimeSeconds / SecondsPerFrame), OutFrames, OutError, FString())) return false;
		}
		return true;
	}

	if (FrameIntervalValue < 0.0)
	{
		OutError = Input(TEXT("invalid_frame_interval"), TEXT("The sequence frame interval must not be negative."),
			TEXT("sequence_capture"), TEXT("/") + FrameIntervalField, Number(FrameIntervalValue), MakeShared<FJsonValueString>(TEXT("frame interval >= 0")));
		return false;
	}
	const int32 StartFrameIndex = bHasStartFrame ? FMath::RoundToInt(StartFrameValue) : 0;
	const int32 FrameInterval = FMath::Max(0, FMath::RoundToInt(FrameIntervalValue));
	for (int32 Index = 0; Index < RequestedCount; ++Index)
	{
		const int32 FrameIndex = StartFrameIndex + FrameInterval * Index;
		if (FrameIndex < 0)
		{
			OutError = Input(TEXT("invalid_frame_index"), TEXT("The generated sequence frame index must not be negative."),
				TEXT("sequence_capture"), StartFrameField.IsEmpty() ? FString() : TEXT("/") + StartFrameField,
				Facts({ { TEXT("sample_index"), Number(Index) }, { TEXT("input_value"), Number(StartFrameValue) }, { TEXT("frame_index"), Number(FrameIndex) } }),
				MakeShared<FJsonValueString>(TEXT("frame index >= 0")));
			return false;
		}
		if (!AddLevelSequenceFrameRequest(MovieScene, static_cast<double>(FrameIndex) * SecondsPerFrame, FrameIndex, OutFrames, OutError, FString())) return false;
	}
	return true;
}

FLevelEditorViewportClient* GetPreferredLevelViewportClient()
{
	if (GEditor)
	{
		if (FViewport* ActiveViewport = GEditor->GetActiveViewport())
		{
			for (FLevelEditorViewportClient* ViewportClient : GEditor->GetLevelViewportClients())
				if (ViewportClient && ViewportClient->Viewport == ActiveViewport) return ViewportClient;
		}
	}
	if (GCurrentLevelEditingViewportClient && GCurrentLevelEditingViewportClient->Viewport) return GCurrentLevelEditingViewportClient;
	if (GEditor)
		for (FLevelEditorViewportClient* ViewportClient : GEditor->GetLevelViewportClients())
			if (ViewportClient && ViewportClient->Viewport) return ViewportClient;
	return nullptr;
}
}
