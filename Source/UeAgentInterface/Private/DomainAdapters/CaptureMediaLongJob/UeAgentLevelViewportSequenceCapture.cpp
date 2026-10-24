#include "DomainAdapters/CaptureMediaLongJob/UeAgentLevelViewportSequenceCapture.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureMediaDiagnostics.h"

#include "Core/EditorViewport/UeAgentViewportViewMode.h"
#include "Core/EditorViewport/UeAgentViewportRenderSize.h"
#include "Core/EditorViewport/UeAgentViewportRealtimeState.h"
#include "Core/AssetPreview/UeAgentAssetPreviewState.h"
#include "Core/AssetPreview/UeAgentAssetPreviewTimeline.h"
#include "Core/AssetPreview/UeAgentAssetPreviewScheduling.h"
#include "Core/EditorCapture/UeAgentSlateWidgetCapture.h"
#include "Core/EditorCapture/UeAgentSlateWindowCapture.h"
#include "Utilities/UeAgentEditorCPUThrottleScope.h"
#include "Slate/SceneViewport.h"
#include "SEditorViewport.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureImageProcessing.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentViewportCapture.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentWorldViewportCaptureMechanics.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "LevelEditorViewport.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonSerializer.h"

#include <openssl/sha.h>

namespace UE::UAI::DomainAdapters::CaptureMediaLongJob
{
	using namespace CaptureDiagnostics;
	namespace
	{
		bool TryIntegerParameter(
			const TSharedPtr<FJsonObject>& Params,
			const TCHAR* Field,
			const int32 DefaultValue,
			const int32 Minimum,
			const int32 Maximum,
			int32& OutValue,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			if (!Params.IsValid() || !Params->HasField(Field))
			{
				OutValue = DefaultValue;
				return true;
			}
			double Value = 0.0;
			const bool bNumber = Params->TryGetNumberField(Field, Value);
			if (!bNumber || !FMath::IsFinite(Value)
				|| Value != FMath::RoundToDouble(Value)
				|| Value < Minimum || Value > Maximum)
			{
				OutError = Input(TEXT("viewport_sequence_parameter_invalid"),
					!bNumber ? TEXT("The viewport sequence parameter must be a number.")
						: !FMath::IsFinite(Value) ? TEXT("The viewport sequence parameter must be finite.")
						: Value != FMath::RoundToDouble(Value) ? TEXT("The viewport sequence parameter must be an integer.")
						: TEXT("The viewport sequence parameter is outside its allowed range."),
					TEXT("viewport_sequence"), FString(TEXT("/")) + Field, bNumber ? Number(Value) : Params->TryGetField(Field),
					Facts({ { TEXT("type"), MakeShared<FJsonValueString>(TEXT("integer")) },
						{ TEXT("minimum"), Number(Minimum) }, { TEXT("maximum"), Number(Maximum) } }));
				return false;
			}
			OutValue = static_cast<int32>(Value);
			return true;
		}

		FString PixelSha256(const TArray<FColor>& Pixels)
		{
			uint8 Digest[SHA256_DIGEST_LENGTH];
			SHA256(
				reinterpret_cast<const uint8*>(Pixels.GetData()),
				Pixels.Num() * sizeof(FColor),
				Digest);
			FString Hex;
			Hex.Reserve(SHA256_DIGEST_LENGTH * 2);
			for (const uint8 Byte : Digest)
			{
				Hex += FString::Printf(TEXT("%02x"), Byte);
			}
			return TEXT("sha256:") + Hex;
		}
	}

	class FLevelViewportSequenceCapture::FImpl final
	{
	public:
		~FImpl()
		{
			RestoreViewport();
			FinishPreview();
			if (!bKeepOutputs)
			{
				for (const FString& Path : OutputPaths)
				{
					IFileManager::Get().Delete(*Path, false, true, true);
				}
			}
		}

		void FinishPreview()
		{
			AssetPreview::FinishPreviewWork(PreviewAssetPath, PreviewTicket);
			PreviewTicket = 0;
		}

		void BeginRealtimeOverride()
		{
			if (!CPUThrottleExemption) CPUThrottleExemption = MakeUnique<EditorRuntime::FScopedCPUThrottleExemption>();
			if (ViewportClient) RealtimeBefore = EditorViewport::ReadRealtimeState(*ViewportClient);
			bRealtimeBefore = RealtimeBefore.bEffective;
			bHadRealtimeOverride = ViewportClient && ViewportClient->HasRealtimeOverride(RealtimeOverrideSystem);
			if (ViewportClient && ((bAssetPreview && bTimelineSampling) || (!bRealtimeBefore && (!bAssetPreview || FrameCount > 1))))
			{
				if (bHadRealtimeOverride) ViewportClient->RemoveRealtimeOverride(RealtimeOverrideSystem, false);
				ViewportClient->AddRealtimeOverride(true, RealtimeOverrideSystem);
				bRealtimeOverrideActive = true;
			}
			if (ViewportClient) ViewportActivity.Begin(*ViewportClient);
			if (bAssetPreview && bTimelineSampling && PreviewContext.Viewport)
			{
				const TWeakPtr<SEditorViewport> WeakViewport = PreviewContext.Viewport;
				PreviewActiveTimer = PreviewContext.Viewport->RegisterActiveTimer(0.f,
					FWidgetActiveTimerDelegate::CreateLambda([WeakViewport](double, float)
					{
						if (const auto Viewport = WeakViewport.Pin()) { Viewport->Invalidate(); return EActiveTimerReturnType::Continue; }
						return EActiveTimerReturnType::Stop;
					}));
			}
		}

		void RestoreViewport()
		{
			CPUThrottleExemption.Reset();
			RenderSize.Restore();
			if (PreviewActiveTimer && PreviewContext.Viewport)
				PreviewContext.Viewport->UnRegisterActiveTimer(PreviewActiveTimer.ToSharedRef());
			PreviewActiveTimer.Reset();
			ContinuousCapture.Restore();
			ViewportActivity.Restore();
			if (bPlaybackChanged)
			{
				TSharedPtr<FJsonObject> Status;
				UE::UAI::Contract::FDiagnostic Error;
				bool Playing = false;
				if (AssetPreview::ExecuteAction(PreviewAsset.Get(), PreviewContext.Target, TEXT("status"), {}, Status, Error)
					&& Status->TryGetBoolField(TEXT("playing"), Playing) && Playing == bExpectedPlaying)
				{
					if (!AssetPreview::ExecuteAction(PreviewAsset.Get(), PreviewContext.Target,
						bWasPlaying ? TEXT("play") : TEXT("pause"), {}, Status, Error, true)) PlaybackRestoreError = Error;
				}
				bPlaybackChanged = false;
			}
			if (ViewportClient && bRealtimeOverrideActive)
			{
				if (ViewportClient->HasRealtimeOverride(RealtimeOverrideSystem))
				{
					ViewportClient->RemoveRealtimeOverride(
						RealtimeOverrideSystem, false);
				}
				if (bHadRealtimeOverride)
				{
					ViewportClient->AddRealtimeOverride(
						bRealtimeBefore, RealtimeOverrideSystem);
				}
				bRealtimeOverrideActive = false;
				const auto Restored = EditorViewport::ReadRealtimeState(*ViewportClient);
				bSequenceRealtimeRestored = Restored.bConfigured == RealtimeBefore.bConfigured
					&& Restored.bEffective == RealtimeBefore.bEffective;
			}
			if (ViewportClient && bRestoreViewMode)
			{
				UeAgentViewportViewMode::RestoreViewportViewModeState(
					ViewportClient, ViewModeState);
				bRestoreViewMode = false;
			}
		}

		bool Tick(
			TSharedPtr<FJsonObject>& OutData,
			bool& bOutComplete,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutData.Reset();
			bOutComplete = false;
			OutError.Reset();
			ON_SCOPE_EXIT { if (!OutError.IsEmpty()) { RestoreViewport(); FinishPreview(); } };
			if (bCancelled)
			{
				OutError = Native(TEXT("viewport_sequence_cancelled"), TEXT("The viewport capture sequence was cancelled."),
					TEXT("viewport_sequence"), TEXT("cancelled"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
				OutError.TargetPath = PreviewAssetPath;
				return false;
			}
			if (bComplete)
			{
				OutData = Result;
				bOutComplete = true;
				return true;
			}
			if (!IsInGameThread())
			{
				OutError = Native(TEXT("viewport_sequence_game_thread_required"), TEXT("Viewport sequence capture requires the game thread."),
					TEXT("viewport_sequence"), TEXT("game_thread"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.TargetPath = PreviewAssetPath;
				return false;
			}
			if (bAssetPreview && !bPreviewStarted)
			{
				if (!AssetPreview::IsPreviewWorkReady(PreviewAssetPath, PreviewTicket)) return true;
				PreviewAsset = StaticLoadObject(UObject::StaticClass(), nullptr, *PreviewAssetPath);
				const FString Target = PreviewContext.Target;
				if (!AssetPreview::ResolveContext(PreviewAsset.Get(), true, PreviewContext, OutError, Target)) return false;
				ViewportClient = PreviewContext.Viewport ? PreviewContext.Viewport->GetViewportClient().Get() : nullptr;
				if (!bPreviewWindowPrepared)
				{
					const auto SceneViewport = PreviewContext.ImageViewport ? PreviewContext.ImageViewport
						: PreviewContext.Viewport ? PreviewContext.Viewport->GetSceneViewport() : nullptr;
					if (SceneViewport)
					{
						if (!EditorCapture::SlateWindow::PrepareViewport(SceneViewport, OutError,
							RequestedRenderSize.X == 0 || RequestedRenderSize.Y == 0)) return false;
					}
					bPreviewWindowPrepared = true;
				}
				if (ViewportClient && ViewportClient->GetViewTransform().IsPlaying()) return true;
				InitialConfiguration = AssetPreview::ReadConfiguration(PreviewContext);
				FJsonSerializer::Serialize(InitialConfiguration.ToSharedRef(), TJsonWriterFactory<>::Create(&LastConfiguration));
				if (!RenderSize.Apply(PreviewContext.ImageViewport ? PreviewContext.ImageViewport
					: PreviewContext.Viewport ? PreviewContext.Viewport->GetSceneViewport() : nullptr, RequestedRenderSize, OutError)) return false;
				BeginRealtimeOverride();
				TSharedPtr<FJsonObject> Status;
				bool HasPlaying = AssetPreview::ExecuteAction(PreviewAsset.Get(), PreviewContext.Target, TEXT("status"), {}, Status, OutError)
					&& Status->TryGetBoolField(TEXT("playing"), bWasPlaying);
				if (!OutError.IsEmpty()) return false;
				auto ApplyPlaybackAction = [&](const TCHAR* Action)
				{
					const bool Applied = AssetPreview::ExecuteAction(PreviewAsset.Get(), PreviewContext.Target, Action, {}, Status, OutError, true);
					UE::UAI::Contract::FDiagnostic ReadError;
					if (HasPlaying && AssetPreview::ExecuteAction(PreviewAsset.Get(), PreviewContext.Target, TEXT("status"), {}, Status, ReadError)
						&& Status->TryGetBoolField(TEXT("playing"), bExpectedPlaying))
						bEverChangedPlayback = bPlaybackChanged = bExpectedPlaying != bWasPlaying;
					return Applied;
				};
				if (bContinuousSampling)
				{
					if (!ContinuousCapture.Begin(PreviewContext, TimelineStartSeconds, TimelineStartSeconds + (FrameCount - 1) * TimelineStepSeconds, OutError)) return false;
					bEverChangedPlayback = true;
				}
				else
				{
					if (bResetBeforeCapture && !ApplyPlaybackAction(TEXT("reset"))) return false;
					if (bTimelineSampling && !ApplyPlaybackAction(TEXT("pause"))) return false;
				}
				StartSeconds = FPlatformTime::Seconds();
				bPreviewStarted = true;
			}
			AssetPreview::FContext CurrentPreview;
			if (bAssetPreview)
			{
				if (!AssetPreview::ResolveContext(PreviewAsset.Get(), false, CurrentPreview, OutError, PreviewContext.Target))
				{
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = PreviewAssetPath;
					return false;
				}
				if (CurrentPreview.Viewport != PreviewContext.Viewport || CurrentPreview.ImageViewport != PreviewContext.ImageViewport
					|| CurrentPreview.PreviewWidget != PreviewContext.PreviewWidget)
				{
					const FString Field = CurrentPreview.Viewport != PreviewContext.Viewport ? TEXT("preview_viewport_matches")
						: CurrentPreview.ImageViewport != PreviewContext.ImageViewport ? TEXT("image_viewport_matches") : TEXT("preview_widget_matches");
					OutError = Native(TEXT("asset_preview_capture_context_changed"), TEXT("The asset preview capture context changed during the sequence."),
						TEXT("viewport_sequence"), Field, MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					OutError.TargetPath = PreviewAssetPath;
					return false;
				}
			}
			if (!bAssetPreview && WorldViewportMechanics::GetPreferredLevelViewportClient() != ViewportClient)
			{
				OutError = Native(TEXT("viewport_sequence_active_viewport_changed"), TEXT("The active level viewport changed during sequence capture."),
					TEXT("viewport_sequence"), TEXT("active_viewport_matches"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.TargetPath = PreviewAssetPath;
				return false;
			}
			if (bContinuousSampling && ViewportClient && ViewportClient->Viewport && LastPreviewRenderFrame != GFrameCounter)
			{
				// Rendering is part of native preview simulation (GPU work, scene-depth
				// collisions and visibility). Do not defer its first view until exposure.
				ViewportClient->Viewport->Invalidate();
				ViewportClient->Viewport->Draw(false);
				LastPreviewRenderFrame = GFrameCounter;
			}
			ViewportActivity.KeepActive();

			++EngineFramesObserved;
			if (FramesUntilSample > 0)
			{
				--FramesUntilSample;
				return true;
			}
			if (bAssetPreview && FPlatformTime::Seconds() - StartSeconds < WarmupSeconds + (bTimelineSampling ? 0 : SheetFrames.Num() * IntervalSeconds))
				return true;
			if (bTimelineSampling && !bTimelineFramePrepared)
			{
				TSharedPtr<FJsonObject> Status;
				const double Time = TimelineStartSeconds + SheetFrames.Num() * TimelineStepSeconds;
				if (bContinuousSampling)
				{
					if (!ContinuousCapture.Advance(PreviewContext, Time, bTimelineFramePrepared, OutError)) return false;
					if (!bTimelineFramePrepared) return true;
				}
				else if (!AssetPreview::ExecuteAction(PreviewAsset.Get(), PreviewContext.Target, TEXT("seek"), Time, Status, OutError)) return false;
				bTimelineFramePrepared = true;
				return true;
			}

			TArray<FColor> Pixels;
			FIntPoint Size;
			TSharedPtr<FJsonObject> CaptureInfo;
			const double SampleSeconds = FPlatformTime::Seconds() - StartSeconds;
			if (bAssetPreview)
			{
				const auto Configuration = AssetPreview::ReadConfiguration(CurrentPreview);
				FString Encoded;
				FJsonSerializer::Serialize(Configuration.ToSharedRef(), TJsonWriterFactory<>::Create(&Encoded));
				if (Encoded != LastConfiguration)
				{
					auto Boundary = MakeShared<FJsonObject>();
					Boundary->SetNumberField(TEXT("first_frame"), SheetFrames.Num());
					Boundary->SetNumberField(TEXT("observed_time_seconds"), SampleSeconds);
					Boundary->SetObjectField(TEXT("configuration"), Configuration);
					ConfigurationChanges.Add(MakeShared<FJsonValueObject>(Boundary));
					LastConfiguration = MoveTemp(Encoded);
				}
			}
			const bool bCaptured = bAssetPreview
				? (PreviewContext.bWidgetPreview && PreviewContext.PreviewWidget
					? UeAgentSlateWidgetCapture::CaptureLiveWidgetPixels(PreviewContext.PreviewWidget.ToSharedRef(), Pixels, Size, OutError)
					: PreviewContext.ImageViewport
					? FViewportCapture::TakeViewportScreenshot(*PreviewContext.ImageViewport, 0, Pixels, Size, CaptureInfo, OutError)
					: FViewportCapture::TakeViewportScreenshot(*ViewportClient, 0, Pixels, Size, CaptureInfo, OutError))
				: FViewportCapture::TakeActiveViewportScreenshotWithWarmup(
					0,
					Pixels,
					Size,
					CaptureInfo,
					OutError);
			if (!bCaptured)
			{
				return false;
			}
			TArray<FColor> Resized;
			FIntPoint ResizedSize;
			const bool bResized = bAssetPreview
				? FCaptureImageProcessing::ResizePixelsToSize(Pixels, Size, OutputSize, Resized, ResizedSize, OutError)
				: FCaptureImageProcessing::ResizePixelsMaxSize(Pixels, Size, MaxSize, Resized, ResizedSize, OutError);
			if (!bResized)
			{
				return false;
			}
			FString ViewModeName;
			if (CaptureInfo.IsValid())
			{
				CaptureInfo->TryGetStringField(TEXT("view_mode_name"), ViewModeName);
				bool bFrameRealtimeRestored = true;
				if (CaptureInfo->TryGetBoolField(TEXT("realtime_state_restored"), bFrameRealtimeRestored))
					bRealtimeStateRestored &= bFrameRealtimeRestored;
			}
			const int32 FrameIndex = SheetFrames.Num();
			if (FrameIndex == 0)
			{
				CommonSize = ResizedSize;
				CommonViewMode = ViewModeName;
				FirstCaptureInfo = CaptureInfo;
			}
			else if (ResizedSize != CommonSize || (!bAssetPreview && ViewModeName != CommonViewMode))
			{
				OutError = Native(TEXT("viewport_sequence_capture_invariant_changed"),
					FString::Printf(TEXT("Captured frame %d differs from the first frame's %s."), FrameIndex, ResizedSize != CommonSize ? TEXT("size") : TEXT("view mode")),
					TEXT("viewport_sequence"), ResizedSize != CommonSize ? TEXT("frame_size") : TEXT("view_mode"),
					ResizedSize != CommonSize ? CaptureDiagnostics::Size(ResizedSize) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(ViewModeName)),
					ResizedSize != CommonSize ? CaptureDiagnostics::Size(CommonSize) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(CommonViewMode)));
				OutError.TargetPath = PreviewAssetPath;
				return false;
			}

			const bool bSingleImage = bSingleAssetImage;
			const FString RawPath = bSingleImage ? SheetPath : FString::Printf(
				TEXT("%s_frame_%03d.%s"), *RawBase, FrameIndex, *Format);
			if (!FCaptureImageProcessing::WriteCompressedImage(
					Resized, ResizedSize, Format, Quality, RawPath, OutError))
			{
				return false;
			}
			OutputPaths.Add(RawPath);
			UWorld* const World = ViewportClient ? ViewportClient->GetWorld() : nullptr;
			const TSharedRef<FJsonObject> Frame = MakeShared<FJsonObject>();
			Frame->SetNumberField(TEXT("frame_index"), FrameIndex);
			Frame->SetNumberField(TEXT("sample_time_seconds"), SampleSeconds);
			if (bAssetPreview && !bTimelineSampling) Frame->SetNumberField(TEXT("requested_time_seconds"), WarmupSeconds + FrameIndex * IntervalSeconds);
			if (bTimelineSampling)
			{
				Frame->SetNumberField(TEXT("requested_asset_time_seconds"), TimelineStartSeconds + FrameIndex * TimelineStepSeconds);
				TSharedPtr<FJsonObject> State;
				if (!AssetPreview::ExecuteAction(PreviewAsset.Get(), PreviewContext.Target, TEXT("status"), {}, State, OutError)) return false;
				Frame->SetObjectField(TEXT("preview_state"), State);
				if (const auto Rate = AssetPreview::PreviewFrameRate(PreviewContext); Rate.IsSet())
				{
					const double RequestedSeconds = TimelineStartSeconds + FrameIndex * TimelineStepSeconds;
					Frame->SetNumberField(TEXT("requested_asset_frame"), RequestedSeconds * Rate->AsDecimal());
					double ActualSeconds = 0;
					if (State->TryGetNumberField(TEXT("local_time_seconds"), ActualSeconds) || State->TryGetNumberField(TEXT("time_seconds"), ActualSeconds))
					{
						Frame->SetNumberField(TEXT("actual_asset_frame"), ActualSeconds * Rate->AsDecimal());
						Frame->SetNumberField(TEXT("frame_deviation"), (ActualSeconds - RequestedSeconds) * Rate->AsDecimal());
					}
				}
			}
			Frame->SetNumberField(
				TEXT("world_time_seconds"), World ? World->GetTimeSeconds() : 0.0);
			Frame->SetNumberField(
				TEXT("engine_frame_number"), static_cast<double>(GFrameCounter));
			Frame->SetStringField(TEXT("view_mode_name"), ViewModeName);
			Frame->SetNumberField(TEXT("width"), ResizedSize.X);
			Frame->SetNumberField(TEXT("height"), ResizedSize.Y);
			Frame->SetNumberField(TEXT("native_width"), Size.X);
			Frame->SetNumberField(TEXT("native_height"), Size.Y);
			if (bAssetPreview)
			{
				Frame->SetNumberField(TEXT("configured_width"), OutputSize.X);
				Frame->SetNumberField(TEXT("configured_height"), OutputSize.Y);
				Frame->SetNumberField(TEXT("requested_render_width"), RequestedRenderSize.X);
				Frame->SetNumberField(TEXT("requested_render_height"), RequestedRenderSize.Y);
			}
			Frame->SetStringField(TEXT("pixel_sha256"), PixelSha256(Resized));
			Frame->SetStringField(TEXT("raw_file_path"), RawPath);
			Frame->SetNumberField(TEXT("sheet_tile_index"), FrameIndex);
			RawFrames.Add(MakeShared<FJsonValueObject>(Frame));
			SheetFrames.Add(MoveTemp(Resized));
			bTimelineFramePrepared = false;
			Labels.Add(FString::Printf(TEXT("F%d"), FrameIndex));

			if (SheetFrames.Num() < FrameCount)
			{
				FramesUntilSample = FMath::Max(0, IntervalFrames - 1);
				return true;
			}

			TSharedPtr<FJsonObject> SheetData;
			if (bSingleImage)
			{
				SheetData = MakeShared<FJsonObject>();
				SheetData->SetNumberField(TEXT("width"), CommonSize.X);
				SheetData->SetNumberField(TEXT("height"), CommonSize.Y);
				SheetData->SetNumberField(TEXT("bytes"), IFileManager::Get().FileSize(*SheetPath));
			}
			else if (!FCaptureImageProcessing::WriteFrameSequenceSheetImage(
					SheetFrames,
					Labels,
					CommonSize,
					Parameters,
					Format,
					Quality,
					SheetPath,
					SheetData,
					OutError))
			{
				return false;
			}
			OutputPaths.Add(SheetPath);
			RestoreViewport();
			if (!bRealtimeStateRestored || !bSequenceRealtimeRestored)
			{
				OutError = Native(TEXT("viewport_sequence_realtime_restore_failed"),
					TEXT("Viewport capture could not restore the original realtime state."), TEXT("capture_restore"), TEXT("realtime_state"),
					Facts({ { TEXT("realtime_state_restored"), MakeShared<FJsonValueBoolean>(bRealtimeStateRestored) },
						{ TEXT("sequence_realtime_restored"), MakeShared<FJsonValueBoolean>(bSequenceRealtimeRestored) } }),
					Facts({ { TEXT("realtime_state_restored"), MakeShared<FJsonValueBoolean>(true) },
						{ TEXT("sequence_realtime_restored"), MakeShared<FJsonValueBoolean>(true) } }));
				return false;
			}

			Result = MakeShared<FJsonObject>();
			if (!bSingleImage) Result->SetStringField(TEXT("frame_sheet_file_path"), SheetPath);
			Result->SetStringField(TEXT("file_path"), SheetPath);
			Result->SetStringField(TEXT("format"), Format);
			Result->SetNumberField(
				TEXT("width"), SheetData->GetNumberField(TEXT("width")));
			Result->SetNumberField(
				TEXT("height"), SheetData->GetNumberField(TEXT("height")));
			Result->SetNumberField(
				TEXT("bytes"), SheetData->GetNumberField(TEXT("bytes")));
			Result->SetArrayField(TEXT("raw_frames"), RawFrames);
			if (!bSingleImage) Result->SetObjectField(TEXT("frame_sheet"), SheetData);
			const TSharedRef<FJsonObject> Metadata = MakeShared<FJsonObject>();
			Metadata->SetStringField(
				TEXT("schema"), TEXT("uai.viewport_sequence_capture.v1"));
			Metadata->SetStringField(TEXT("sampling_clock"), bContinuousSampling ? TEXT("native_continuous_playback") : bTimelineSampling ? TEXT("native_timeline") : bAssetPreview ? TEXT("realtime_clock") : TEXT("engine_frame"));
			if (bSingleAssetImage && RawFrames.Num() == 1)
			{
				// Single-image packaging omits raw_frames. Preserve the state at the
				// actual exposure, separately from the restored end_preview_state.
				auto Sample = MakeShared<FJsonObject>();
				const auto CapturedFrame = RawFrames[0]->AsObject();
				for (const TCHAR* Field : {TEXT("frame_index"), TEXT("engine_frame_number"), TEXT("sample_time_seconds"),
					TEXT("width"), TEXT("height"), TEXT("native_width"), TEXT("native_height"),
					TEXT("configured_width"), TEXT("configured_height"), TEXT("requested_render_width"), TEXT("requested_render_height"),
					TEXT("requested_time_seconds"), TEXT("requested_asset_time_seconds"), TEXT("requested_asset_frame"),
					TEXT("actual_asset_frame"), TEXT("frame_deviation"), TEXT("preview_state")})
					if (const auto* Value = CapturedFrame->Values.Find(Field)) Sample->SetField(Field, *Value);
				Metadata->SetObjectField(TEXT("sample"), Sample);
			}
			if (bAssetPreview)
			{
				Metadata->SetStringField(TEXT("source"), TEXT("asset_preview"));
				Metadata->SetStringField(TEXT("asset_path"), PreviewAsset->GetPathName());
				Metadata->SetStringField(TEXT("target"), PreviewContext.Target);
				Metadata->SetNumberField(TEXT("interval_seconds"), IntervalSeconds);
				Metadata->SetArrayField(TEXT("configuration_boundaries"), ConfigurationChanges);
				Metadata->SetObjectField(TEXT("initial_configuration"), InitialConfiguration);
				TSharedPtr<FJsonObject> Playback;
				UE::UAI::Contract::FDiagnostic PlaybackError;
				if (AssetPreview::ExecuteAction(PreviewAsset.Get(), PreviewContext.Target, TEXT("status"), {}, Playback, PlaybackError))
					Metadata->SetObjectField(TEXT("end_preview_state"), Playback);
				else Metadata->SetObjectField(TEXT("end_preview_state_error"), PlaybackError.ToPublicJson());
				Metadata->SetBoolField(TEXT("playback_changed_by_capture"), bEverChangedPlayback);
				Metadata->SetNumberField(TEXT("warmup_seconds"), WarmupSeconds);
				Metadata->SetBoolField(TEXT("reset_before_capture"), bResetBeforeCapture || bContinuousSampling);
				if (!PlaybackRestoreError.IsEmpty()) Metadata->SetObjectField(TEXT("playback_restore_error"), PlaybackRestoreError.ToPublicJson());
			}
			Metadata->SetNumberField(TEXT("frame_count"), FrameCount);
			if (bSingleImage) Metadata->SetNumberField(TEXT("sample_time_seconds"), SampleSeconds);
			Metadata->SetNumberField(TEXT("interval_frames"), IntervalFrames);
			Metadata->SetNumberField(TEXT("warmup_frames"), WarmupFrames);
			Metadata->SetNumberField(
				TEXT("engine_frames_observed"), EngineFramesObserved);
			Metadata->SetStringField(TEXT("view_mode_name"), CommonViewMode);
			Metadata->SetNumberField(TEXT("frame_width"), CommonSize.X);
			Metadata->SetNumberField(TEXT("frame_height"), CommonSize.Y);
			Metadata->SetBoolField(TEXT("viewport_state_restored"), true);
			Metadata->SetBoolField(
				TEXT("realtime_state_restored"),
				bRealtimeStateRestored && bSequenceRealtimeRestored);
			if (FirstCaptureInfo.IsValid())
			{
				Metadata->SetObjectField(TEXT("first_frame"), FirstCaptureInfo);
			}
			Result->SetObjectField(TEXT("capture"), Metadata);
			const int64 SheetBytes = IFileManager::Get().FileSize(*SheetPath);
			if (SheetBytes <= 0)
			{
				OutError = Native(TEXT("viewport_sequence_sheet_empty"), TEXT("The captured frame-sheet file is missing or empty."),
					TEXT("viewport_sequence"), TEXT("file_size"), Number(SheetBytes), MakeShared<FJsonValueString>(TEXT("positive byte count")));
				OutError.TargetPath = SheetPath;
				return false;
			}
			bKeepOutputs = true;
			bComplete = true;
			FinishPreview();
			OutData = Result;
			bOutComplete = true;
			return true;
		}

		TSharedPtr<FJsonObject> Parameters;
		FEditorViewportClient* ViewportClient = nullptr;
		AssetPreview::FContext PreviewContext;
		TWeakObjectPtr<UObject> PreviewAsset;
		FString PreviewAssetPath;
		int64 PreviewTicket = 0;
		bool bPreviewStarted = false;
		bool bPreviewWindowPrepared = false;
		bool bTimelineSampling = false;
		bool bContinuousSampling = false;
		AssetPreview::FContinuousCapture ContinuousCapture;
		TUniquePtr<EditorRuntime::FScopedCPUThrottleExemption> CPUThrottleExemption;
		TSharedPtr<FActiveTimerHandle> PreviewActiveTimer;
		uint64 LastPreviewRenderFrame = MAX_uint64;
		bool bTimelineFramePrepared = false;
		bool bResetBeforeCapture = false;
		bool bWasPlaying = false;
		bool bExpectedPlaying = false;
		bool bPlaybackChanged = false;
		bool bEverChangedPlayback = false;
		double WarmupSeconds = 0;
		double TimelineStartSeconds = 0;
		double TimelineStepSeconds = 0;
		UE::UAI::Contract::FDiagnostic PlaybackRestoreError;
		FIntPoint OutputSize = FIntPoint::ZeroValue;
		FIntPoint RequestedRenderSize = FIntPoint::ZeroValue;
		EditorViewport::FScopedRenderSize RenderSize;
		EditorViewport::FScopedViewportActivity ViewportActivity;
		EditorViewport::FRealtimeState RealtimeBefore;
		double IntervalSeconds = 0.0;
		bool bAssetPreview = false;
		bool bSingleAssetImage = false;
		UeAgentViewportViewMode::FViewportViewModeState ViewModeState;
		FText RealtimeOverrideSystem;
		FString SheetPath;
		FString RawBase;
		FString Format;
		FString CommonViewMode;
		TArray<TArray<FColor>> SheetFrames;
		TArray<FString> Labels;
		TArray<TSharedPtr<FJsonValue>> RawFrames;
		TArray<FString> OutputPaths;
		TSharedPtr<FJsonObject> FirstCaptureInfo;
		TSharedPtr<FJsonObject> InitialConfiguration;
		FString LastConfiguration;
		TArray<TSharedPtr<FJsonValue>> ConfigurationChanges;
		TSharedPtr<FJsonObject> Result;
		FIntPoint CommonSize = FIntPoint::ZeroValue;
		double StartSeconds = FPlatformTime::Seconds();
		int32 FrameCount = 0;
		int32 IntervalFrames = 0;
		int32 WarmupFrames = 0;
		int32 Quality = 0;
		int32 MaxSize = 0;
		int32 FramesUntilSample = 0;
		int32 EngineFramesObserved = 0;
		bool bRestoreViewMode = false;
		bool bRealtimeBefore = false;
		bool bHadRealtimeOverride = false;
		bool bRealtimeOverrideActive = false;
		bool bRealtimeStateRestored = true;
		bool bSequenceRealtimeRestored = true;
		bool bComplete = false;
		bool bCancelled = false;
		bool bKeepOutputs = false;
	};

	FLevelViewportSequenceCapture::FLevelViewportSequenceCapture(
		TUniquePtr<FImpl> InImpl)
		: Impl(MoveTemp(InImpl))
	{
	}

	FLevelViewportSequenceCapture::~FLevelViewportSequenceCapture() = default;

	bool FLevelViewportSequenceCapture::TryCreate(
		const FUeAgentRequestContext& Context,
		TSharedPtr<FLevelViewportSequenceCapture>& OutCapture,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutCapture.Reset();
		OutError.Reset();
		if (!IsInGameThread())
		{
			OutError = Native(TEXT("viewport_sequence_game_thread_required"), TEXT("Viewport sequence capture must be created on the game thread."),
				TEXT("viewport_sequence"), TEXT("game_thread"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return false;
		}
		TUniquePtr<FImpl> Impl = MakeUnique<FImpl>();
		Impl->Parameters = Context.Params;
		int32 FrameSheetPadding = 0;
		if (!TryIntegerParameter(
				Context.Params, TEXT("frame_count"), 4, 1, MAX_int32,
				Impl->FrameCount, OutError)
			|| !TryIntegerParameter(
				Context.Params, TEXT("interval_frames"), 1, 1, MAX_int32,
				Impl->IntervalFrames, OutError)
			|| !TryIntegerParameter(
				Context.Params, TEXT("warmup_frames"), 0, 0, MAX_int32,
				Impl->WarmupFrames, OutError)
			|| !TryIntegerParameter(
				Context.Params, TEXT("quality"), 90, 1, 100,
				Impl->Quality, OutError)
			|| !TryIntegerParameter(
				Context.Params, TEXT("max_size"), 0, 1, MAX_int32,
				Impl->MaxSize, OutError)
			|| !TryIntegerParameter(
				Context.Params, TEXT("frame_sheet_padding"), 12, 0, MAX_int32,
				FrameSheetPadding, OutError))
		{
			return false;
		}
		Impl->Format = TEXT("png");
		if (Context.Params.IsValid()
			&& Context.Params->HasField(TEXT("format"))
			&& !Context.Params->TryGetStringField(TEXT("format"), Impl->Format))
		{
			OutError = Input(TEXT("viewport_sequence_parameter_invalid"), TEXT("The viewport sequence image format must be a string."),
				TEXT("viewport_sequence"), TEXT("/format"), Context.Params->TryGetField(TEXT("format")), MakeShared<FJsonValueString>(TEXT("string")));
			return false;
		}
		Impl->Format.ToLowerInline();
		if (Impl->Format != TEXT("png") && Impl->Format != TEXT("jpg"))
		{
			OutError = Input(TEXT("viewport_sequence_parameter_invalid"), TEXT("The viewport sequence image format is unsupported."),
				TEXT("viewport_sequence"), TEXT("/format"), Context.Params->TryGetField(TEXT("format")), MakeShared<FJsonValueString>(TEXT("png or jpg")));
			return false;
		}

		bool bHasRequestedViewMode = false;
		UeAgentViewportViewMode::FResolvedViewportViewMode RequestedViewMode;
		Context.Params->TryGetBoolField(TEXT("asset_preview"), Impl->bAssetPreview);
		// Asset capture mode selects single/sequence; rendering comes from the
		// already applied preview workspace, not Level viewport parameter aliases.
		if (!Impl->bAssetPreview && !UeAgentViewportViewMode::TryResolveRequestedViewportViewMode(
				Context.Params,
				bHasRequestedViewMode,
				RequestedViewMode,
				OutError))
		{
			// The capture policy preserves this prepared parameter's original source separately.
			OutError.SourceKind = TEXT("command_input");
			OutError.Source.bCommandInput = true;
			OutError.Source.JsonPath = FString(TEXT("/")) + OutError.PropertyPath;
			OutError.SemanticPointer = OutError.Source.JsonPath;
			return false;
		}
		FString SelectionReason;
		if (Impl->bAssetPreview)
		{
			FString Mode = TEXT("single");
			Context.Params->TryGetStringField(TEXT("mode"), Mode);
			Impl->bSingleAssetImage = Mode == TEXT("single");
			const FString AssetPath = Context.Params->GetStringField(TEXT("asset_path"));
			Impl->PreviewAssetPath = AssetPath;
			Impl->PreviewAsset = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);
			FString Target;
			Context.Params->TryGetStringField(TEXT("target"), Target);
			if (!AssetPreview::ResolveContext(Impl->PreviewAsset.Get(), true, Impl->PreviewContext, OutError, Target)) return false;
			Impl->ViewportClient = Impl->PreviewContext.Viewport ? Impl->PreviewContext.Viewport->GetViewportClient().Get() : nullptr;
			Impl->OutputSize = FIntPoint(Context.Params->GetIntegerField(TEXT("capture_width")), Context.Params->GetIntegerField(TEXT("capture_height")));
			Impl->RequestedRenderSize = FIntPoint(Context.Params->GetIntegerField(TEXT("render_width")), Context.Params->GetIntegerField(TEXT("render_height")));
			Impl->IntervalSeconds = Context.Params->GetNumberField(TEXT("interval_seconds"));
			Context.Params->TryGetBoolField(TEXT("timeline_sampling"), Impl->bTimelineSampling);
			Context.Params->TryGetBoolField(TEXT("continuous_sampling"), Impl->bContinuousSampling);
			Context.Params->TryGetBoolField(TEXT("reset_before_capture"), Impl->bResetBeforeCapture);
			Context.Params->TryGetNumberField(TEXT("warmup_seconds"), Impl->WarmupSeconds);
			Context.Params->TryGetNumberField(TEXT("timeline_start_seconds"), Impl->TimelineStartSeconds);
			Context.Params->TryGetNumberField(TEXT("timeline_step_seconds"), Impl->TimelineStepSeconds);
			bool bFrameUnits = false;
			Context.Params->TryGetBoolField(TEXT("timeline_frame_units"), bFrameUnits);
			if (Impl->bTimelineSampling && bFrameUnits)
			{
				const auto Rate = AssetPreview::PreviewFrameRate(Impl->PreviewContext);
				if (!Rate.IsSet() || Rate->Numerator <= 0 || Rate->Denominator <= 0)
				{
					OutError = Native(TEXT("preview_native_frame_rate_unavailable"), TEXT("The asset preview has no valid native frame rate; use seconds for timeline sampling."),
						TEXT("viewport_sequence"), TEXT("frame_rate"),
						Rate.IsSet() ? Facts({ { TEXT("numerator"), Number(Rate->Numerator) }, { TEXT("denominator"), Number(Rate->Denominator) } }) : nullptr,
						MakeShared<FJsonValueString>(TEXT("positive numerator and denominator")));
					OutError.TargetPath = Impl->PreviewAssetPath;
					return false;
				}
				Impl->TimelineStartSeconds = Context.Params->GetNumberField(TEXT("timeline_start_frame")) / Rate->AsDecimal();
				Impl->TimelineStepSeconds = Context.Params->GetNumberField(TEXT("timeline_step_frames")) / Rate->AsDecimal();
			}
			if (Impl->bTimelineSampling && !Impl->bContinuousSampling && Impl->PreviewContext.ViewportType == TEXT("SNiagaraSystemViewport"))
			{
				OutError = Input(TEXT("preview_niagara_direct_timeline_unsupported"), TEXT("Niagara timeline capture requires continuous sampling to preserve simulation history."),
					TEXT("viewport_sequence"), TEXT("/continuous_sampling"), MakeShared<FJsonValueBoolean>(Impl->bContinuousSampling), MakeShared<FJsonValueBoolean>(true));
				OutError.TargetPath = Impl->PreviewAssetPath;
				return false;
			}
			Impl->WarmupFrames = 1;
			Impl->InitialConfiguration = AssetPreview::ReadConfiguration(Impl->PreviewContext);
			FJsonSerializer::Serialize(Impl->InitialConfiguration.ToSharedRef(), TJsonWriterFactory<>::Create(&Impl->LastConfiguration));
		}
		else
		{
			auto* LevelViewportClient = WorldViewportMechanics::GetPreferredLevelViewportClient(&SelectionReason);
			if (LevelViewportClient && !WorldViewportMechanics::PrepareLevelViewportForCapture(LevelViewportClient, OutError)) return false;
			Impl->ViewportClient = LevelViewportClient;
		}
		if ((!Impl->ViewportClient || !Impl->ViewportClient->Viewport) && !Impl->PreviewContext.ImageViewport
			&& !(Impl->PreviewContext.bWidgetPreview && Impl->PreviewContext.PreviewWidget))
		{
			OutError = Native(TEXT("no_level_editor_viewport"), TEXT("No supported viewport or preview widget is available for sequence capture."),
				TEXT("viewport_sequence"), TEXT("capture_viewport_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.TargetPath = Impl->PreviewAssetPath;
			OutError.InternalContext = SelectionReason;
			return false;
		}
		Impl->RealtimeOverrideSystem = NSLOCTEXT(
			"UeAgentInterface",
			"ViewportSequenceRealtimeOverrideSystem",
			"UeAgentInterfaceViewportSequence");
		if (!Impl->bAssetPreview) Impl->BeginRealtimeOverride();
		if (bHasRequestedViewMode)
		{
			Impl->ViewModeState =
				UeAgentViewportViewMode::CaptureViewportViewModeState(
					Impl->ViewportClient);
			Impl->bRestoreViewMode = true;
			UeAgentViewportViewMode::ApplyResolvedViewportViewMode(
				Impl->ViewportClient, RequestedViewMode);
		}

		if (!Context.Params->TryGetStringField(
				TEXT("frame_sheet_output_file"), Impl->SheetPath)
			|| Impl->SheetPath.IsEmpty())
		{
			Context.Params->TryGetStringField(TEXT("output_file"), Impl->SheetPath);
		}
		if (Impl->SheetPath.IsEmpty())
		{
			OutError = Native(TEXT("viewport_sequence_output_missing"), TEXT("The prepared viewport sequence capture has no frame_sheet_output_file or output_file."),
				TEXT("viewport_sequence"), FString(),
				Facts({ { TEXT("frame_sheet_output_file"), Context.Params->TryGetField(TEXT("frame_sheet_output_file")) },
					{ TEXT("output_file"), Context.Params->TryGetField(TEXT("output_file")) } }),
				MakeShared<FJsonValueString>(TEXT("non-empty output file path")));
			return false;
		}
		Context.Params->SetNumberField(
			TEXT("frame_sheet_padding"), FrameSheetPadding);
		Impl->RawBase = FPaths::Combine(
			FPaths::GetPath(Impl->SheetPath),
			FPaths::GetBaseFilename(Impl->SheetPath));
		Impl->FramesUntilSample = Impl->WarmupFrames;
		Impl->StartSeconds = FPlatformTime::Seconds();
		if (Impl->bAssetPreview) Impl->PreviewTicket = AssetPreview::EnqueuePreviewWork(Impl->PreviewAssetPath);
		OutCapture = MakeShareable(
			new FLevelViewportSequenceCapture(MoveTemp(Impl)));
		return true;
	}

	bool FLevelViewportSequenceCapture::Tick(
		TSharedPtr<FJsonObject>& OutData,
		bool& bOutComplete,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!Impl.IsValid())
		{
			OutError = Native(TEXT("viewport_sequence_state_unavailable"), TEXT("The viewport sequence capture state is unavailable."),
				TEXT("viewport_sequence"), TEXT("capture_state_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			bOutComplete = false;
			OutData.Reset();
			return false;
		}
		return Impl->Tick(OutData, bOutComplete, OutError);
	}

	void FLevelViewportSequenceCapture::Cancel()
	{
		if (Impl.IsValid())
		{
			Impl->bCancelled = true;
			Impl->RestoreViewport();
			Impl.Reset();
		}
	}
}
