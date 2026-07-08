// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commands/EditorViewport/UeAgentEditorViewportSequenceFrameOptions.h"

#include "Dom/JsonValue.h"

namespace UeAgentEditorViewportSequenceFrameOptions
{
	FUnifiedSequenceFrameOptionsReport::FUnifiedSequenceFrameOptionsReport()
		: VisualOptionsApplied(MakeShared<FJsonObject>())
		, PreviewStateApplied(MakeShared<FJsonObject>())
		, bHadVisualOptions(false)
		, bHadPreviewState(false)
	{
	}

	bool FUnifiedSequenceFrameOptionsReport::HasAnyInput() const
	{
		return bHadVisualOptions || bHadPreviewState;
	}

	static TArray<TSharedPtr<FJsonValue>> MakeStringArrayJson(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		Result.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			Result.Add(MakeShared<FJsonValueString>(Value));
		}
		return Result;
	}

	static TSharedPtr<FJsonObject> GetObjectField(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName)
	{
		const TSharedPtr<FJsonObject>* ObjectPtr = nullptr;
		if (Params.IsValid() && Params->TryGetObjectField(FieldName, ObjectPtr) && ObjectPtr && ObjectPtr->IsValid())
		{
			return *ObjectPtr;
		}
		return nullptr;
	}

	static bool HasUserFields(const TSharedPtr<FJsonObject>& Object)
	{
		return Object.IsValid() && Object->Values.Num() > 0;
	}

	static FString NormalizeKey(const FString& Key)
	{
		FString Normalized = Key.TrimStartAndEnd().ToLower();
		Normalized.ReplaceInline(TEXT("-"), TEXT("_"));
		Normalized.ReplaceInline(TEXT(" "), TEXT("_"));
		return Normalized;
	}

	static void AddUniqueString(TArray<FString>& Values, const FString& Value)
	{
		if (!Value.IsEmpty())
		{
			Values.AddUnique(Value);
		}
	}

	static bool CopyNestedFieldToTopLevel(
		const TSharedPtr<FJsonObject>& Params,
		const TSharedPtr<FJsonObject>& Source,
		const TCHAR* SourceKey,
		const TCHAR* DestinationKey,
		const TCHAR* SourceObjectName,
		TSharedPtr<FJsonObject>& Applied,
		TArray<FString>& Conflicts)
	{
		if (!Params.IsValid() || !Source.IsValid())
		{
			return false;
		}

		const TSharedPtr<FJsonValue>* SourceValue = Source->Values.Find(SourceKey);
		if (!SourceValue || !SourceValue->IsValid())
		{
			return false;
		}

		if (Params->HasField(DestinationKey))
		{
			Conflicts.AddUnique(FString::Printf(TEXT("%s.%s->%s"), SourceObjectName, SourceKey, DestinationKey));
			return true;
		}

		Params->SetField(DestinationKey, *SourceValue);
		if (Applied.IsValid())
		{
			Applied->SetField(DestinationKey, *SourceValue);
		}
		return true;
	}

	static void MarkRecognized(TSet<FString>& RecognizedKeys, const TCHAR* Key)
	{
		RecognizedKeys.Add(NormalizeKey(Key));
	}

	static void CopyIfPresent(
		const TSharedPtr<FJsonObject>& Params,
		const TSharedPtr<FJsonObject>& Source,
		const TCHAR* SourceKey,
		const TCHAR* DestinationKey,
		const TCHAR* SourceObjectName,
		TSet<FString>& RecognizedKeys,
		TSharedPtr<FJsonObject>& Applied,
		TArray<FString>& Conflicts)
	{
		MarkRecognized(RecognizedKeys, SourceKey);
		CopyNestedFieldToTopLevel(Params, Source, SourceKey, DestinationKey, SourceObjectName, Applied, Conflicts);
	}

	static void CopySameKey(
		const TSharedPtr<FJsonObject>& Params,
		const TSharedPtr<FJsonObject>& Source,
		const TCHAR* Key,
		const TCHAR* SourceObjectName,
		TSet<FString>& RecognizedKeys,
		TSharedPtr<FJsonObject>& Applied,
		TArray<FString>& Conflicts)
	{
		CopyIfPresent(Params, Source, Key, Key, SourceObjectName, RecognizedKeys, Applied, Conflicts);
	}

	static void CopyCommonVisualOutputOptions(
		const TSharedPtr<FJsonObject>& Params,
		const TSharedPtr<FJsonObject>& VisualOptions,
		FUnifiedSequenceFrameOptionsReport& Report,
		TSet<FString>& RecognizedVisualKeys)
	{
		static const TCHAR* CommonKeys[] =
		{
			TEXT("target"),
			TEXT("format"),
			TEXT("quality"),
			TEXT("max_size"),
			TEXT("width"),
			TEXT("height"),
			TEXT("create_frame_sheet"),
			TEXT("frame_sheet"),
			TEXT("sequence_frame_image"),
			TEXT("write_individual_frames"),
			TEXT("keep_individual_frames"),
			TEXT("output_individual_frames"),
			TEXT("frame_sheet_padding"),
			TEXT("sheet_padding"),
			TEXT("frame_sheet_label_scale"),
			TEXT("sheet_label_scale"),
			TEXT("frame_sheet_max_size"),
			TEXT("sheet_max_size")
		};
		for (const TCHAR* Key : CommonKeys)
		{
			CopySameKey(Params, VisualOptions, Key, TEXT("visual_options"), RecognizedVisualKeys, Report.VisualOptionsApplied, Report.Conflicts);
		}
	}

	static void CopyCommonPreviewFrameOptions(
		const TSharedPtr<FJsonObject>& Params,
		const TSharedPtr<FJsonObject>& PreviewState,
		FUnifiedSequenceFrameOptionsReport& Report,
		TSet<FString>& RecognizedPreviewKeys)
	{
		static const TCHAR* CommonKeys[] =
		{
			TEXT("frames"),
			TEXT("count"),
			TEXT("frame_count"),
			TEXT("sample_count"),
			TEXT("start_frame_index"),
			TEXT("first_frame_index"),
			TEXT("frame_index"),
			TEXT("frame"),
			TEXT("display_frame"),
			TEXT("time_seconds"),
			TEXT("start_time_seconds"),
			TEXT("first_time_seconds"),
			TEXT("interval_seconds"),
			TEXT("time_interval_seconds"),
			TEXT("frame_interval"),
			TEXT("frame_stride")
		};
		for (const TCHAR* Key : CommonKeys)
		{
			CopySameKey(Params, PreviewState, Key, TEXT("preview_state"), RecognizedPreviewKeys, Report.PreviewStateApplied, Report.Conflicts);
		}

		CopyIfPresent(Params, PreviewState, TEXT("start_frame"), TEXT("start_frame_index"), TEXT("preview_state"), RecognizedPreviewKeys, Report.PreviewStateApplied, Report.Conflicts);
		CopyIfPresent(Params, PreviewState, TEXT("seconds"), TEXT("time_seconds"), TEXT("preview_state"), RecognizedPreviewKeys, Report.PreviewStateApplied, Report.Conflicts);
	}

	static void MarkVisualViewportOnlyKeys(TSet<FString>& RecognizedVisualKeys)
	{
		static const TCHAR* ViewportOnlyKeys[] =
		{
			TEXT("view_mode"),
			TEXT("view_mode_value"),
			TEXT("grid"),
			TEXT("bounds"),
			TEXT("pivot"),
			TEXT("selection"),
			TEXT("mode_widgets"),
			TEXT("collision"),
			TEXT("game_view"),
			TEXT("realtime"),
			TEXT("overlays"),
			TEXT("ignore_unsupported")
		};
		for (const TCHAR* Key : ViewportOnlyKeys)
		{
			MarkRecognized(RecognizedVisualKeys, Key);
		}
	}

	static void CollectIgnoredFields(
		const TSharedPtr<FJsonObject>& Source,
		const TSet<FString>& RecognizedKeys,
		TArray<FString>& OutIgnored)
	{
		if (!Source.IsValid())
		{
			return;
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Source->Values)
		{
			if (!RecognizedKeys.Contains(NormalizeKey(Pair.Key)))
			{
				AddUniqueString(OutIgnored, Pair.Key);
			}
		}
	}

	void ApplyUnifiedSequenceFrameOptionsToParams(
		const TSharedPtr<FJsonObject>& Params,
		const ESequenceFrameCommandKind Kind,
		FUnifiedSequenceFrameOptionsReport& Report)
	{
		Report = FUnifiedSequenceFrameOptionsReport();
		if (!Params.IsValid())
		{
			return;
		}

		const TSharedPtr<FJsonObject> VisualOptions = GetObjectField(Params, TEXT("visual_options"));
		const TSharedPtr<FJsonObject> PreviewState = GetObjectField(Params, TEXT("preview_state"));
		Report.bHadVisualOptions = HasUserFields(VisualOptions);
		Report.bHadPreviewState = HasUserFields(PreviewState);

		TSet<FString> RecognizedVisualKeys;
		TSet<FString> RecognizedPreviewKeys;

		if (Report.bHadVisualOptions)
		{
			CopyCommonVisualOutputOptions(Params, VisualOptions, Report, RecognizedVisualKeys);

			if (Kind == ESequenceFrameCommandKind::Niagara)
			{
				CopySameKey(Params, VisualOptions, TEXT("frame_capture_renderer"), TEXT("visual_options"), RecognizedVisualKeys, Report.VisualOptionsApplied, Report.Conflicts);
				CopyIfPresent(Params, VisualOptions, TEXT("renderer"), TEXT("frame_capture_renderer"), TEXT("visual_options"), RecognizedVisualKeys, Report.VisualOptionsApplied, Report.Conflicts);
				CopyIfPresent(Params, VisualOptions, TEXT("capture_renderer"), TEXT("frame_capture_renderer"), TEXT("visual_options"), RecognizedVisualKeys, Report.VisualOptionsApplied, Report.Conflicts);
				CopySameKey(Params, VisualOptions, TEXT("camera_fov"), TEXT("visual_options"), RecognizedVisualKeys, Report.VisualOptionsApplied, Report.Conflicts);
				CopySameKey(Params, VisualOptions, TEXT("capture_fov"), TEXT("visual_options"), RecognizedVisualKeys, Report.VisualOptionsApplied, Report.Conflicts);
				CopySameKey(Params, VisualOptions, TEXT("camera_distance"), TEXT("visual_options"), RecognizedVisualKeys, Report.VisualOptionsApplied, Report.Conflicts);
				CopySameKey(Params, VisualOptions, TEXT("capture_distance"), TEXT("visual_options"), RecognizedVisualKeys, Report.VisualOptionsApplied, Report.Conflicts);
				CopySameKey(Params, VisualOptions, TEXT("camera_distance_scale"), TEXT("visual_options"), RecognizedVisualKeys, Report.VisualOptionsApplied, Report.Conflicts);
				CopySameKey(Params, VisualOptions, TEXT("capture_distance_scale"), TEXT("visual_options"), RecognizedVisualKeys, Report.VisualOptionsApplied, Report.Conflicts);
				CopySameKey(Params, VisualOptions, TEXT("camera_target_z_offset"), TEXT("visual_options"), RecognizedVisualKeys, Report.VisualOptionsApplied, Report.Conflicts);
				CopySameKey(Params, VisualOptions, TEXT("camera_height_offset"), TEXT("visual_options"), RecognizedVisualKeys, Report.VisualOptionsApplied, Report.Conflicts);
				CopySameKey(Params, VisualOptions, TEXT("camera_side_offset"), TEXT("visual_options"), RecognizedVisualKeys, Report.VisualOptionsApplied, Report.Conflicts);
				CopySameKey(Params, VisualOptions, TEXT("scene_capture_warmup_captures"), TEXT("visual_options"), RecognizedVisualKeys, Report.VisualOptionsApplied, Report.Conflicts);
				CopySameKey(Params, VisualOptions, TEXT("render_warmup_captures"), TEXT("visual_options"), RecognizedVisualKeys, Report.VisualOptionsApplied, Report.Conflicts);
				CopySameKey(Params, VisualOptions, TEXT("wait_for_render_dependencies"), TEXT("visual_options"), RecognizedVisualKeys, Report.VisualOptionsApplied, Report.Conflicts);
			}
			else if (Kind == ESequenceFrameCommandKind::LevelSequence)
			{
				CopySameKey(Params, VisualOptions, TEXT("fallback_to_viewport_camera"), TEXT("visual_options"), RecognizedVisualKeys, Report.VisualOptionsApplied, Report.Conflicts);
			}
			else if (Kind == ESequenceFrameCommandKind::ActorProbe)
			{
				CopySameKey(Params, VisualOptions, TEXT("warmup_frames"), TEXT("visual_options"), RecognizedVisualKeys, Report.VisualOptionsApplied, Report.Conflicts);
				CopySameKey(Params, VisualOptions, TEXT("render_warmup_frames"), TEXT("visual_options"), RecognizedVisualKeys, Report.VisualOptionsApplied, Report.Conflicts);
				CopySameKey(Params, VisualOptions, TEXT("tick_editor_world"), TEXT("visual_options"), RecognizedVisualKeys, Report.VisualOptionsApplied, Report.Conflicts);
				CopySameKey(Params, VisualOptions, TEXT("tick_editor_world_during_warmup"), TEXT("visual_options"), RecognizedVisualKeys, Report.VisualOptionsApplied, Report.Conflicts);
				CopySameKey(Params, VisualOptions, TEXT("editor_world_tick_frames"), TEXT("visual_options"), RecognizedVisualKeys, Report.VisualOptionsApplied, Report.Conflicts);
				CopySameKey(Params, VisualOptions, TEXT("editor_world_tick_delta_seconds"), TEXT("visual_options"), RecognizedVisualKeys, Report.VisualOptionsApplied, Report.Conflicts);
				CopySameKey(Params, VisualOptions, TEXT("create_sheet"), TEXT("visual_options"), RecognizedVisualKeys, Report.VisualOptionsApplied, Report.Conflicts);
			}
			else if (Kind == ESequenceFrameCommandKind::IKProbeCandidates || Kind == ESequenceFrameCommandKind::IKProbeReviewFrames)
			{
				CopySameKey(Params, VisualOptions, TEXT("frame_width"), TEXT("visual_options"), RecognizedVisualKeys, Report.VisualOptionsApplied, Report.Conflicts);
				CopySameKey(Params, VisualOptions, TEXT("frame_height"), TEXT("visual_options"), RecognizedVisualKeys, Report.VisualOptionsApplied, Report.Conflicts);
				CopySameKey(Params, VisualOptions, TEXT("mesh_capture_bounds_padding"), TEXT("visual_options"), RecognizedVisualKeys, Report.VisualOptionsApplied, Report.Conflicts);
				CopySameKey(Params, VisualOptions, TEXT("bounds_padding"), TEXT("visual_options"), RecognizedVisualKeys, Report.VisualOptionsApplied, Report.Conflicts);
				if (Kind == ESequenceFrameCommandKind::IKProbeReviewFrames)
				{
					CopySameKey(Params, VisualOptions, TEXT("rerender"), TEXT("visual_options"), RecognizedVisualKeys, Report.VisualOptionsApplied, Report.Conflicts);
					CopySameKey(Params, VisualOptions, TEXT("recapture"), TEXT("visual_options"), RecognizedVisualKeys, Report.VisualOptionsApplied, Report.Conflicts);
					CopySameKey(Params, VisualOptions, TEXT("rerender_selected_frames"), TEXT("visual_options"), RecognizedVisualKeys, Report.VisualOptionsApplied, Report.Conflicts);
					CopySameKey(Params, VisualOptions, TEXT("use_cached_pixels"), TEXT("visual_options"), RecognizedVisualKeys, Report.VisualOptionsApplied, Report.Conflicts);
				}
			}

			CollectIgnoredFields(VisualOptions, RecognizedVisualKeys, Report.VisualOptionsIgnored);
		}

		if (Report.bHadPreviewState)
		{
			const bool bSupportsTimelinePreviewState =
				Kind == ESequenceFrameCommandKind::Niagara ||
				Kind == ESequenceFrameCommandKind::LevelSequence ||
				Kind == ESequenceFrameCommandKind::AnimSequence ||
				Kind == ESequenceFrameCommandKind::IKProbeCandidates;
			if (bSupportsTimelinePreviewState)
			{
				CopyCommonPreviewFrameOptions(Params, PreviewState, Report, RecognizedPreviewKeys);
			}

			if (Kind == ESequenceFrameCommandKind::Niagara)
			{
				CopySameKey(Params, PreviewState, TEXT("target_frame"), TEXT("preview_state"), RecognizedPreviewKeys, Report.PreviewStateApplied, Report.Conflicts);
				CopyIfPresent(Params, PreviewState, TEXT("pause_at_frame"), TEXT("target_frame"), TEXT("preview_state"), RecognizedPreviewKeys, Report.PreviewStateApplied, Report.Conflicts);
				CopyIfPresent(Params, PreviewState, TEXT("tick_delta_seconds"), TEXT("preview_tick_delta_seconds"), TEXT("preview_state"), RecognizedPreviewKeys, Report.PreviewStateApplied, Report.Conflicts);
				CopySameKey(Params, PreviewState, TEXT("preview_tick_delta_seconds"), TEXT("preview_state"), RecognizedPreviewKeys, Report.PreviewStateApplied, Report.Conflicts);
				CopySameKey(Params, PreviewState, TEXT("advance_mode"), TEXT("preview_state"), RecognizedPreviewKeys, Report.PreviewStateApplied, Report.Conflicts);
				CopyIfPresent(Params, PreviewState, TEXT("restore_interactive_preview"), TEXT("restore_preview_after_capture"), TEXT("preview_state"), RecognizedPreviewKeys, Report.PreviewStateApplied, Report.Conflicts);
				CopyIfPresent(Params, PreviewState, TEXT("reset"), TEXT("reset_preview"), TEXT("preview_state"), RecognizedPreviewKeys, Report.PreviewStateApplied, Report.Conflicts);
				CopyIfPresent(Params, PreviewState, TEXT("pause"), TEXT("pause_preview_after_advance"), TEXT("preview_state"), RecognizedPreviewKeys, Report.PreviewStateApplied, Report.Conflicts);
			}
			else if (Kind == ESequenceFrameCommandKind::LevelSequence || Kind == ESequenceFrameCommandKind::AnimSequence || Kind == ESequenceFrameCommandKind::IKProbeCandidates)
			{
				CopyIfPresent(Params, PreviewState, TEXT("target_frame"), TEXT("frame_index"), TEXT("preview_state"), RecognizedPreviewKeys, Report.PreviewStateApplied, Report.Conflicts);
				CopyIfPresent(Params, PreviewState, TEXT("pause_at_frame"), TEXT("frame_index"), TEXT("preview_state"), RecognizedPreviewKeys, Report.PreviewStateApplied, Report.Conflicts);
				if (Kind == ESequenceFrameCommandKind::IKProbeCandidates)
				{
					CopyIfPresent(Params, PreviewState, TEXT("sequence_frame_index"), TEXT("frame_index"), TEXT("preview_state"), RecognizedPreviewKeys, Report.PreviewStateApplied, Report.Conflicts);
					CopySameKey(Params, PreviewState, TEXT("evaluation_frame"), TEXT("preview_state"), RecognizedPreviewKeys, Report.PreviewStateApplied, Report.Conflicts);
				}
			}
			else if (Kind == ESequenceFrameCommandKind::IKProbeReviewFrames)
			{
				CopySameKey(Params, PreviewState, TEXT("frames"), TEXT("preview_state"), RecognizedPreviewKeys, Report.PreviewStateApplied, Report.Conflicts);
				CopySameKey(Params, PreviewState, TEXT("frame_index"), TEXT("preview_state"), RecognizedPreviewKeys, Report.PreviewStateApplied, Report.Conflicts);
				CopySameKey(Params, PreviewState, TEXT("frame_indices"), TEXT("preview_state"), RecognizedPreviewKeys, Report.PreviewStateApplied, Report.Conflicts);
				CopySameKey(Params, PreviewState, TEXT("frame_indexes"), TEXT("preview_state"), RecognizedPreviewKeys, Report.PreviewStateApplied, Report.Conflicts);
				CopySameKey(Params, PreviewState, TEXT("numbers"), TEXT("preview_state"), RecognizedPreviewKeys, Report.PreviewStateApplied, Report.Conflicts);
				CopySameKey(Params, PreviewState, TEXT("selections"), TEXT("preview_state"), RecognizedPreviewKeys, Report.PreviewStateApplied, Report.Conflicts);
				CopyIfPresent(Params, PreviewState, TEXT("frame"), TEXT("frame_index"), TEXT("preview_state"), RecognizedPreviewKeys, Report.PreviewStateApplied, Report.Conflicts);
				CopyIfPresent(Params, PreviewState, TEXT("target_frame"), TEXT("frame_index"), TEXT("preview_state"), RecognizedPreviewKeys, Report.PreviewStateApplied, Report.Conflicts);
				CopyIfPresent(Params, PreviewState, TEXT("pause_at_frame"), TEXT("frame_index"), TEXT("preview_state"), RecognizedPreviewKeys, Report.PreviewStateApplied, Report.Conflicts);
			}

			CollectIgnoredFields(PreviewState, RecognizedPreviewKeys, Report.PreviewStateIgnored);
		}
	}

	void AttachUnifiedSequenceFrameOptionsReport(
		const TSharedPtr<FJsonObject>& OutData,
		const FUnifiedSequenceFrameOptionsReport& Report)
	{
		if (!OutData.IsValid() || !Report.HasAnyInput())
		{
			return;
		}

		TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
		Summary->SetBoolField(TEXT("visual_options_requested"), Report.bHadVisualOptions);
		Summary->SetBoolField(TEXT("preview_state_requested"), Report.bHadPreviewState);
		Summary->SetStringField(TEXT("conflict_policy"), TEXT("existing_top_level_param_wins"));
		Summary->SetArrayField(TEXT("conflicts"), MakeStringArrayJson(Report.Conflicts));

		OutData->SetObjectField(TEXT("unified_sequence_frame_options"), Summary);
		OutData->SetObjectField(TEXT("visual_options_applied"), Report.VisualOptionsApplied.IsValid() ? Report.VisualOptionsApplied : MakeShared<FJsonObject>());
		OutData->SetObjectField(TEXT("preview_state_applied"), Report.PreviewStateApplied.IsValid() ? Report.PreviewStateApplied : MakeShared<FJsonObject>());
		OutData->SetArrayField(TEXT("visual_options_ignored"), MakeStringArrayJson(Report.VisualOptionsIgnored));
		OutData->SetArrayField(TEXT("preview_state_ignored"), MakeStringArrayJson(Report.PreviewStateIgnored));
	}
}
