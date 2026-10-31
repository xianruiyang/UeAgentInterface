#include "DomainAdapters/CaptureMediaLongJob/UeAgentViewportCapture.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/EditorCapture/UeAgentSlateWindowCapture.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureMediaDiagnostics.h"
#include "Core/EditorViewport/UeAgentViewportRealtimeState.h"

#include "DomainAdapters/CaptureMediaLongJob/UeAgentWorldViewportCaptureMechanics.h"
#include "Core/EditorViewport/UeAgentViewportViewMode.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureImageProcessing.h"
#include "Module/UeAgentInterfaceLogger.h"
#include "UeAgentInterfaceSettings.h"
#include "Utilities/UeAgentJsonDiagnostics.h"

#include "Components/SceneCaptureComponent2D.h"
#include "Editor.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "LevelEditorViewport.h"
#include "Misc/Paths.h"
#include "RenderingThread.h"

#include "Dom/JsonObject.h"
#include "Misc/Guid.h"

namespace UE::UAI::DomainAdapters::CaptureMediaLongJob
{
	using namespace CaptureDiagnostics;
	namespace
	{
		bool TryCapture(
			const TSharedPtr<FJsonObject>& Parameters,
			const FString& Command,
			bool (*CaptureFunction)(
				const FUeAgentRequestContext&,
				TSharedPtr<FJsonObject>&,
				UE::UAI::Contract::FDiagnostic&),
			TSharedPtr<FJsonObject>& OutData,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			FUeAgentRequestContext Context;
			Context.RequestId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
			Context.Command = Command;
			Context.Params = Parameters;
			return CaptureFunction(Context, OutData, OutError);
		}
	}

	bool TryCaptureViewportImage(
		const TSharedPtr<FJsonObject>& Parameters,
		const FString& AssetNotFoundError,
		TSharedPtr<FJsonObject>& OutData,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		(void)AssetNotFoundError;
		return TryCapture(
			Parameters,
			TEXT("screenshot_viewport"),
			&FViewportCapture::Capture,
			OutData,
			OutError);
	}

	bool TryCaptureViewportBufferImage(
		const TSharedPtr<FJsonObject>& Parameters,
		const FString& AssetNotFoundError,
		TSharedPtr<FJsonObject>& OutData,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		(void)AssetNotFoundError;
		return TryCapture(
			Parameters,
			TEXT("screenshot_viewport_buffer"),
			&FViewportCapture::CaptureBuffer,
			OutData,
			OutError);
	}
}

namespace
{
	bool JsonTryGetString(
		const TSharedPtr<FJsonObject>& Object,
		const FString& Key,
		FString& OutValue)
	{
		return Object.IsValid() && Object->TryGetStringField(Key, OutValue);
	}

	bool JsonTryGetNumber(
		const TSharedPtr<FJsonObject>& Object,
		const FString& Key,
		double& OutValue)
	{
		return Object.IsValid() && Object->TryGetNumberField(Key, OutValue);
	}

	bool JsonTryGetBool(
		const TSharedPtr<FJsonObject>& Object,
		const FString& Key,
		bool& OutValue)
	{
		return Object.IsValid() && Object->TryGetBoolField(Key, OutValue);
	}
}

bool UE::UAI::DomainAdapters::CaptureMediaLongJob::FViewportCapture::TakeActiveViewportScreenshot(TArray<FColor>& OutPixels, FIntPoint& OutSize, UE::UAI::Contract::FDiagnostic& OutError)
{
	TSharedPtr<FJsonObject> CaptureInfo;
	return TakeActiveViewportScreenshotWithWarmup(
		0, OutPixels, OutSize, CaptureInfo, OutError);
}

bool UE::UAI::DomainAdapters::CaptureMediaLongJob::FViewportCapture::TakeActiveViewportScreenshotWithWarmup(int32 WarmupFrames, TArray<FColor>& OutPixels, FIntPoint& OutSize, TSharedPtr<FJsonObject>& OutCaptureInfo, UE::UAI::Contract::FDiagnostic& OutError)
{
	FString SelectionReason;
	FLevelEditorViewportClient* LVC = UE::UAI::DomainAdapters::CaptureMediaLongJob::WorldViewportMechanics::GetPreferredLevelViewportClient(&SelectionReason);
	if (!LVC || !LVC->Viewport)
	{
		OutError = Native(TEXT("no_level_editor_viewport"), TEXT("No level editor viewport is available for image capture."),
			TEXT("capture_native"), TEXT("viewport_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
		OutError.InternalContext = SelectionReason;
		return false;
	}
	UE::UAI::EditorCapture::SlateWindow::FScopedViewportExtent ExtentScope;
	if (!ExtentScope.Prepare(WorldViewportMechanics::GetLevelSceneViewport(LVC), nullptr, FString(), OutError)) return false;
	if (!TakeViewportScreenshot(*LVC, WarmupFrames, OutPixels, OutSize, OutCaptureInfo, OutError)) return false;
	ExtentScope.Restore();
	OutCaptureInfo->SetObjectField(TEXT("viewport_initialization"), ExtentScope.Describe());
	OutCaptureInfo->SetStringField(TEXT("render_path"), TEXT("level_viewport_backbuffer"));
	OutCaptureInfo->SetObjectField(TEXT("viewport"), UE::UAI::DomainAdapters::CaptureMediaLongJob::WorldViewportMechanics::MakeViewportClientJson(LVC, SelectionReason));
	return true;
}

bool UE::UAI::DomainAdapters::CaptureMediaLongJob::FViewportCapture::TakeViewportScreenshot(
	FEditorViewportClient& Client, int32 WarmupFrames, TArray<FColor>& OutPixels,
	FIntPoint& OutSize, TSharedPtr<FJsonObject>& OutCaptureInfo, UE::UAI::Contract::FDiagnostic& OutError)
{
	FEditorViewportClient* LVC = &Client;
	if (!LVC->Viewport)
	{
		OutError = Native(TEXT("preview_viewport_unavailable"), TEXT("The editor viewport client has no renderable viewport."),
			TEXT("capture_native"), TEXT("viewport_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
		return false;
	}

	FViewport* Viewport = LVC->Viewport;
	const auto ExposureBefore = LVC->ExposureSettings;
	const bool bGameViewBefore = LVC->IsInGameView();
	const bool bLightingBefore = LVC->EngineShowFlags.Lighting;
	const bool bPostProcessingBefore = LVC->EngineShowFlags.PostProcessing;
	const bool bEyeAdaptationBefore = LVC->EngineShowFlags.EyeAdaptation;
	const bool bTonemapperBefore = LVC->EngineShowFlags.Tonemapper;
	const auto RealtimeBefore = UE::UAI::EditorViewport::ReadRealtimeState(*LVC);
	const bool bRealtimeBefore = LVC->IsRealtime();
	const FText ScreenshotRealtimeOverrideSystem = NSLOCTEXT("UeAgentInterface", "ScreenshotRealtimeOverrideSystem", "UeAgentInterfaceScreenshot");
	const bool bNeedTemporaryRealtime = !LVC->IsRealtime();
	const bool bHadRealtimeOverride =
		LVC->HasRealtimeOverride(ScreenshotRealtimeOverrideSystem);
	struct FScopedRealtimeStateRestore final
	{
		FEditorViewportClient* Client = nullptr;
		FText System;
		bool bActive = false;
		bool bHadOverride = false;
		bool bRealtimeBefore = false;
		bool bRestored = false;

		~FScopedRealtimeStateRestore()
		{
			RestoreNow();
		}

		void RestoreNow()
		{
			if (!Client || bRestored)
			{
				return;
			}
			if (bActive)
			{
				if (Client->HasRealtimeOverride(System))
				{
					Client->RemoveRealtimeOverride(System, false);
				}
				if (bHadOverride)
				{
					Client->AddRealtimeOverride(bRealtimeBefore, System);
				}
			}
			bRestored = true;
		}
	};
	FScopedRealtimeStateRestore RealtimeRestore{
		LVC,
		ScreenshotRealtimeOverrideSystem,
		bNeedTemporaryRealtime,
		bHadRealtimeOverride,
		bRealtimeBefore,
		false};
	if (bNeedTemporaryRealtime)
	{
		if (LVC->HasRealtimeOverride(ScreenshotRealtimeOverrideSystem))
		{
			LVC->RemoveRealtimeOverride(ScreenshotRealtimeOverrideSystem, false);
		}
		LVC->AddRealtimeOverride(true, ScreenshotRealtimeOverrideSystem);
	}

	LVC->Invalidate();
	if (!TakeViewportScreenshot(*Viewport, WarmupFrames, OutPixels, OutSize, OutCaptureInfo, OutError)) return false;
	RealtimeRestore.RestoreNow();
	OutCaptureInfo->SetBoolField(TEXT("final_lit_composition"), UeAgentViewportViewMode::IsFinalLitCompositionViewMode(LVC->GetViewMode()));
	OutCaptureInfo->SetStringField(TEXT("view_mode_name"), UeAgentViewportViewMode::ViewModeIndexToStableName(LVC->GetViewMode()));
	OutCaptureInfo->SetBoolField(TEXT("temporary_realtime_override"), bNeedTemporaryRealtime);
	OutCaptureInfo->SetBoolField(TEXT("realtime_before_capture"), bRealtimeBefore);
	OutCaptureInfo->SetBoolField(TEXT("realtime_configured"), RealtimeBefore.bConfigured);
	OutCaptureInfo->SetBoolField(TEXT("realtime_effective_before_capture"), RealtimeBefore.bEffective);
	if (!RealtimeBefore.OverrideReason.IsEmpty()) OutCaptureInfo->SetStringField(TEXT("realtime_override_reason"), RealtimeBefore.OverrideReason);
	OutCaptureInfo->SetBoolField(TEXT("realtime_after_capture"), LVC->IsRealtime());
	const auto RealtimeAfter = UE::UAI::EditorViewport::ReadRealtimeState(*LVC);
	OutCaptureInfo->SetBoolField(TEXT("realtime_state_restored"),
		RealtimeAfter.bConfigured == RealtimeBefore.bConfigured && RealtimeAfter.bEffective == RealtimeBefore.bEffective);
	// These are the editor display controls used for this draw, not the resolved
	// scene post process or the renderer's temporal exposure history. Observe only.
	const auto DisplayState = MakeShared<FJsonObject>();
	DisplayState->SetBoolField(TEXT("game_view"), bGameViewBefore);
	DisplayState->SetBoolField(TEXT("exposure_override_enabled"), ExposureBefore.bFixed);
	if (ExposureBefore.bFixed) DisplayState->SetNumberField(TEXT("fixed_ev100"), ExposureBefore.FixedEV100);
	DisplayState->SetBoolField(TEXT("lighting"), bLightingBefore);
	DisplayState->SetBoolField(TEXT("post_processing"), bPostProcessingBefore);
	DisplayState->SetBoolField(TEXT("eye_adaptation"), bEyeAdaptationBefore);
	DisplayState->SetBoolField(TEXT("tonemapper"), bTonemapperBefore);
	OutCaptureInfo->SetObjectField(TEXT("display_state"), DisplayState);
	OutCaptureInfo->SetBoolField(TEXT("display_state_unchanged"),
		LVC->IsInGameView() == bGameViewBefore
		&& LVC->ExposureSettings.bFixed == ExposureBefore.bFixed
		&& LVC->ExposureSettings.FixedEV100 == ExposureBefore.FixedEV100
		&& LVC->EngineShowFlags.Lighting == bLightingBefore
		&& LVC->EngineShowFlags.PostProcessing == bPostProcessingBefore
		&& LVC->EngineShowFlags.EyeAdaptation == bEyeAdaptationBefore
		&& LVC->EngineShowFlags.Tonemapper == bTonemapperBefore);
	return true;
}

bool UE::UAI::DomainAdapters::CaptureMediaLongJob::FViewportCapture::TakeViewportScreenshot(
	FViewport& NativeViewport, int32 WarmupFrames, TArray<FColor>& OutPixels,
	FIntPoint& OutSize, TSharedPtr<FJsonObject>& OutCaptureInfo, UE::UAI::Contract::FDiagnostic& OutError)
{
	FViewport* Viewport = &NativeViewport;
	int32 DrawsPerformed = 0;
	for (int32 FrameIndex = 0; FrameIndex < WarmupFrames; ++FrameIndex)
	{
		Viewport->Invalidate();
		Viewport->Draw(/*bShouldPresent*/ false);
		DrawsPerformed++;
	}

	Viewport->Invalidate();
	Viewport->Draw(/*bShouldPresent*/ false);
	DrawsPerformed++;

	const FIntPoint Size = Viewport->GetSizeXY();
	if (Size.X <= 0 || Size.Y <= 0)
	{
		OutError = Native(TEXT("invalid_viewport_size"), TEXT("The viewport dimensions must be positive before pixels can be captured."),
			TEXT("capture_native"), TEXT("dimensions"), CaptureDiagnostics::Size(Size), MakeShared<FJsonValueString>(TEXT("positive width and height")));
		return false;
	}

	TArray<FColor> Pixels;
	if (static_cast<int64>(Size.X) * Size.Y > MAX_int32)
	{
		OutError = Native(TEXT("viewport_pixel_count_overflow"), TEXT("The viewport pixel count exceeds the supported array size."),
			TEXT("capture_native"), TEXT("pixel_count"), Number(static_cast<int64>(Size.X) * Size.Y), Number(MAX_int32));
		return false;
	}
	Pixels.AddUninitialized(Size.X * Size.Y);
	// ReadPixels enqueues the surface read after the draws and flushes the render
	// command queue itself. Per-draw flushes only serialize CPU and GPU work.
	if (!Viewport->ReadPixels(Pixels))
	{
		OutError = Native(TEXT("read_pixels_failed"), TEXT("The viewport could not read its backbuffer pixels."),
			TEXT("capture_native"), TEXT("pixels_read"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
		return false;
	}

	// An Editor backbuffer is an opaque screen image, not an alpha mask.
	for (FColor& Pixel : Pixels) Pixel.A = 255;
	OutPixels = MoveTemp(Pixels);
	OutSize = Size;

	OutCaptureInfo = MakeShared<FJsonObject>();
	OutCaptureInfo->SetStringField(TEXT("schema"), TEXT("uai.viewport_capture.v1"));
	OutCaptureInfo->SetStringField(TEXT("render_path"), TEXT("editor_viewport_backbuffer"));
	OutCaptureInfo->SetNumberField(TEXT("native_width"), Size.X);
	OutCaptureInfo->SetNumberField(TEXT("native_height"), Size.Y);
	OutCaptureInfo->SetNumberField(TEXT("warmup_frames_requested"), WarmupFrames);
	OutCaptureInfo->SetNumberField(TEXT("draws_performed"), DrawsPerformed);
	OutCaptureInfo->SetNumberField(TEXT("display_gamma"), Viewport->GetDisplayGamma());
	OutCaptureInfo->SetBoolField(TEXT("scene_hdr_enabled"), Viewport->GetSceneHDREnabled());
	return true;
}

FString UE::UAI::DomainAdapters::CaptureMediaLongJob::FViewportCapture::MakeShotPath(const FString& Ext)
{
	const UUeAgentInterfaceSettings* Settings = UUeAgentInterfaceSettings::Get();
	const FString Hostname = FPlatformProcess::ComputerName();
	const FString Ts = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S_%s"));

	const FString RootRel = Settings ? Settings->ArtifactsDirRelative : TEXT("Saved/RemoteArtifacts");
	const FString ShotsDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), RootRel / TEXT("Shots"));
	return FPaths::Combine(ShotsDir, FString::Printf(TEXT("shot_%s_%s.%s"), *Hostname, *Ts, *Ext));
}

bool UE::UAI::DomainAdapters::CaptureMediaLongJob::FViewportCapture::Capture(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError)
{
	TArray<TSharedPtr<FJsonValue>> OutArtifacts;
	if (!OutData.IsValid())
	{
		OutData = MakeShared<FJsonObject>();
	}

	FString Format = TEXT("jpg");
	int32 Quality = 85;
	int32 MaxSize = 0;
	const bool bHasExplicitFormat = JsonTryGetString(Ctx.Params, TEXT("format"), Format);
	double QD = (double)Quality;
	if (JsonTryGetNumber(Ctx.Params, TEXT("quality"), QD))
	{
		Quality = FMath::Clamp((int32)QD, 1, 100);
	}
	double MSD = (double)MaxSize;
	if (JsonTryGetNumber(Ctx.Params, TEXT("max_size"), MSD))
	{
		MaxSize = FMath::Max((int32)MSD, 1);
	}
	int32 WarmupFrames = 2;
	double WarmupFramesD = (double)WarmupFrames;
	const bool bPrimaryWarmup = JsonTryGetNumber(Ctx.Params, TEXT("warmup_frames"), WarmupFramesD);
	if (bPrimaryWarmup || JsonTryGetNumber(Ctx.Params, TEXT("render_warmup_frames"), WarmupFramesD))
	{
		if (!FMath::IsFinite(WarmupFramesD) || WarmupFramesD < 0.0
            || WarmupFramesD > static_cast<double>(MAX_int32))
        {
            OutError = Input(TEXT("warmup_frames_invalid"), TEXT("The warmup frame count must be finite and within the supported nonnegative range."),
                TEXT("capture_prepare"), bPrimaryWarmup ? TEXT("/warmup_frames") : TEXT("/render_warmup_frames"), Number(WarmupFramesD),
                Facts({ { TEXT("minimum"), Number(0) }, { TEXT("maximum"), Number(MAX_int32) } }));
            return false;
        }
        WarmupFrames = static_cast<int32>(WarmupFramesD);
	}
	Format = Format.ToLower();
	if (Format == TEXT("jpeg"))
	{
		Format = TEXT("jpg");
	}

	FString OutputFile;
	FString ScreenshotOutPath;
	if (JsonTryGetString(Ctx.Params, TEXT("output_file"), OutputFile) && !OutputFile.TrimStartAndEnd().IsEmpty())
	{
		ScreenshotOutPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(OutputFile);
		FString OutputExt = FPaths::GetExtension(ScreenshotOutPath).ToLower();
		if (OutputExt == TEXT("jpeg"))
		{
			OutputExt = TEXT("jpg");
		}
		if (OutputExt == TEXT("png") || OutputExt == TEXT("jpg") || OutputExt == TEXT("webp"))
		{
			if (!bHasExplicitFormat)
			{
				Format = OutputExt;
			}
			else if (OutputExt != Format)
			{
				OutError = Input(TEXT("output_file_extension_mismatch"), TEXT("The output file extension does not match the requested image format."),
					TEXT("capture_prepare"), TEXT("/output_file"), MakeShared<FJsonValueString>(OutputExt), MakeShared<FJsonValueString>(Format));
				FUeAgentInterfaceLogger::Log(TEXT("CmdScreenshotViewport req=%s failed: %s output=%s format=%s"), *Ctx.RequestId, *OutError.ToPublicDetail(), *ScreenshotOutPath, *Format);
				return false;
			}
		}
		else if (OutputExt.IsEmpty())
		{
			ScreenshotOutPath += FString::Printf(TEXT(".%s"), *Format);
		}
		else
		{
			OutError = Input(TEXT("unsupported_output_file_extension"), TEXT("The output file extension is not supported for viewport capture."),
				TEXT("capture_prepare"), TEXT("/output_file"), MakeShared<FJsonValueString>(OutputExt), MakeShared<FJsonValueString>(TEXT("png, jpg/jpeg, or webp")));
			FUeAgentInterfaceLogger::Log(TEXT("CmdScreenshotViewport req=%s failed: %s output=%s"), *Ctx.RequestId, *OutError.ToPublicDetail(), *ScreenshotOutPath);
			return false;
		}
	}
	else
	{
		ScreenshotOutPath = MakeShotPath(Format);
	}

	if (Format != TEXT("png") && Format != TEXT("jpg") && Format != TEXT("webp"))
	{
		OutError = Input(TEXT("unsupported_format"), TEXT("The image format is not supported for viewport capture."),
			TEXT("capture_prepare"), TEXT("/format"), Ctx.Params.IsValid() ? Ctx.Params->TryGetField(TEXT("format")) : nullptr,
			MakeShared<FJsonValueString>(TEXT("png, jpg/jpeg, or webp")));
		FUeAgentInterfaceLogger::Log(TEXT("CmdScreenshotViewport req=%s failed: %s format=%s"), *Ctx.RequestId, *OutError.ToPublicDetail(), *Format);
		return false;
	}

	bool bHasRequestedViewMode = false;
	UeAgentViewportViewMode::FResolvedViewportViewMode RequestedViewMode;
	if (!UeAgentViewportViewMode::TryResolveRequestedViewportViewMode(Ctx.Params, bHasRequestedViewMode, RequestedViewMode, OutError))
	{
		// This caller passes command parameters; the resolver supplies the actual selected alias.
		OutError.SourceKind = TEXT("command_input");
		OutError.Source.bCommandInput = true;
		OutError.Source.JsonPath = FString(TEXT("/")) + OutError.PropertyPath;
		OutError.SemanticPointer = OutError.Source.JsonPath;
		FUeAgentInterfaceLogger::Log(TEXT("CmdScreenshotViewport req=%s failed: %s"), *Ctx.RequestId, *OutError.ToPublicDetail());
		return false;
	}

	bool bRestoreViewMode = true;
	JsonTryGetBool(Ctx.Params, TEXT("restore_view_mode"), bRestoreViewMode);
	JsonTryGetBool(Ctx.Params, TEXT("restore_view_mode_after_capture"), bRestoreViewMode);

	struct FScopedScreenshotViewModeRestore
	{
		FLevelEditorViewportClient* Client = nullptr;
		UeAgentViewportViewMode::FViewportViewModeState PreviousState;
		bool bNeedsRestore = false;

		~FScopedScreenshotViewModeRestore()
		{
			RestoreNow();
		}

		void RestoreNow()
		{
			if (bNeedsRestore && Client)
			{
				UeAgentViewportViewMode::RestoreViewportViewModeState(Client, PreviousState);
				bNeedsRestore = false;
			}
		}
	};

	FScopedScreenshotViewModeRestore ViewModeRestore;
	FLevelEditorViewportClient* RequestedLVC = nullptr;
	if (bHasRequestedViewMode)
	{
		FString SelectionReason;
		RequestedLVC = UE::UAI::DomainAdapters::CaptureMediaLongJob::WorldViewportMechanics::GetPreferredLevelViewportClient(&SelectionReason);
		if (!RequestedLVC || !RequestedLVC->Viewport)
		{
			OutError = Native(TEXT("no_level_editor_viewport"), TEXT("No level editor viewport is available to apply the requested view mode."),
				TEXT("capture_native"), TEXT("viewport_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.InternalContext = SelectionReason;
			FUeAgentInterfaceLogger::Log(TEXT("CmdScreenshotViewport req=%s failed: %s"), *Ctx.RequestId, *OutError.ToPublicDetail());
			return false;
		}

		OutData->SetObjectField(TEXT("requested_view_mode"), UeAgentViewportViewMode::MakeResolvedViewportViewModeJson(RequestedViewMode));
		OutData->SetObjectField(TEXT("view_mode_before_capture"), UeAgentViewportViewMode::MakeCurrentViewportViewModeJson(RequestedLVC));
		OutData->SetBoolField(TEXT("restore_view_mode"), bRestoreViewMode);

		ViewModeRestore.Client = RequestedLVC;
		ViewModeRestore.PreviousState = UeAgentViewportViewMode::CaptureViewportViewModeState(RequestedLVC);
		ViewModeRestore.bNeedsRestore = bRestoreViewMode;
		UeAgentViewportViewMode::ApplyResolvedViewportViewMode(RequestedLVC, RequestedViewMode);
		OutData->SetObjectField(TEXT("view_mode_applied"), UeAgentViewportViewMode::MakeCurrentViewportViewModeJson(RequestedLVC));
	}

	FUeAgentInterfaceLogger::Log(TEXT("CmdScreenshotViewport req=%s begin format=%s quality=%d max_size=%d view_mode=%s"),
		*Ctx.RequestId,
		*Format,
		Quality,
		MaxSize,
		bHasRequestedViewMode ? *RequestedViewMode.Key : TEXT("current"));

	TArray<FColor> Pixels;
	FIntPoint Size(0, 0);
	TSharedPtr<FJsonObject> CaptureInfo;
	if (!TakeActiveViewportScreenshotWithWarmup(
			WarmupFrames, Pixels, Size, CaptureInfo, OutError))
	{
		FUeAgentInterfaceLogger::Log(TEXT("CmdScreenshotViewport req=%s failed: %s"), *Ctx.RequestId, *OutError.ToPublicDetail());
		return false;
	}

	TArray<FColor> Resized;
	FIntPoint ResizedSize(0, 0);
	if (!UE::UAI::DomainAdapters::CaptureMediaLongJob::FCaptureImageProcessing::ResizePixelsMaxSize(Pixels, Size, MaxSize, Resized, ResizedSize, OutError))
	{
		FUeAgentInterfaceLogger::Log(TEXT("CmdScreenshotViewport req=%s failed: %s"), *Ctx.RequestId, *OutError.ToPublicDetail());
		return false;
	}

	if (!UE::UAI::DomainAdapters::CaptureMediaLongJob::FCaptureImageProcessing::WriteCompressedImage(Resized, ResizedSize, Format, Quality, ScreenshotOutPath, OutError))
	{
		FUeAgentInterfaceLogger::Log(TEXT("CmdScreenshotViewport req=%s failed: %s"), *Ctx.RequestId, *OutError.ToPublicDetail());
		return false;
	}

	const int64 Bytes = IFileManager::Get().FileSize(*ScreenshotOutPath);
	bool bCaptureFinalLitComposition = bHasRequestedViewMode ? RequestedViewMode.bFinalLitComposition : true;
	if (CaptureInfo.IsValid())
	{
		CaptureInfo->TryGetBoolField(TEXT("final_lit_composition"), bCaptureFinalLitComposition);
	}

	OutData->SetStringField(TEXT("file_path"), ScreenshotOutPath);
	OutData->SetNumberField(TEXT("width"), ResizedSize.X);
	OutData->SetNumberField(TEXT("height"), ResizedSize.Y);
	OutData->SetNumberField(TEXT("bytes"), (double)Bytes);
	OutData->SetStringField(TEXT("format"), Format);
	OutData->SetStringField(TEXT("capture_mode"), bHasRequestedViewMode ? TEXT("level_viewport_view_mode") : TEXT("level_viewport_lit"));
	OutData->SetBoolField(TEXT("final_lit_composition"), bCaptureFinalLitComposition);
	OutData->SetBoolField(TEXT("scene_capture_buffer"), false);
	OutData->SetNumberField(TEXT("warmup_frames_requested"), WarmupFrames);
	if (CaptureInfo.IsValid())
	{
		OutData->SetObjectField(TEXT("capture"), CaptureInfo);
		const TSharedPtr<FJsonObject>* ViewportInfo = nullptr;
		if (CaptureInfo->TryGetObjectField(TEXT("viewport"), ViewportInfo) && ViewportInfo && ViewportInfo->IsValid())
		{
			OutData->SetObjectField(TEXT("viewport"), *ViewportInfo);
		}
	}

	TSharedPtr<FJsonObject> Artifact = MakeShared<FJsonObject>();
	Artifact->SetStringField(TEXT("kind"), TEXT("screenshot"));
	Artifact->SetStringField(TEXT("file_path"), ScreenshotOutPath);
	Artifact->SetNumberField(TEXT("bytes"), (double)Bytes);
	Artifact->SetStringField(TEXT("mime"), Format == TEXT("png") ? TEXT("image/png") : (Format == TEXT("webp") ? TEXT("image/webp") : TEXT("image/jpeg")));
	Artifact->SetNumberField(TEXT("width"), ResizedSize.X);
	Artifact->SetNumberField(TEXT("height"), ResizedSize.Y);
	Artifact->SetStringField(TEXT("capture_mode"), bHasRequestedViewMode ? TEXT("level_viewport_view_mode") : TEXT("level_viewport_lit"));
	Artifact->SetBoolField(TEXT("final_lit_composition"), bCaptureFinalLitComposition);
	OutArtifacts.Add(MakeShared<FJsonValueObject>(Artifact));

	if (bHasRequestedViewMode && bRestoreViewMode && RequestedLVC)
	{
		ViewModeRestore.RestoreNow();
		OutData->SetObjectField(TEXT("view_mode_after_restore"), UeAgentViewportViewMode::MakeCurrentViewportViewModeJson(RequestedLVC));
	}
	else if (bHasRequestedViewMode)
	{
		OutData->SetBoolField(TEXT("view_mode_left_applied"), true);
	}

	FUeAgentInterfaceLogger::Log(TEXT("CmdScreenshotViewport req=%s ok path=%s size=%dx%d bytes=%lld"),
		*Ctx.RequestId,
		*ScreenshotOutPath,
		ResizedSize.X,
		ResizedSize.Y,
		(long long)Bytes);

	return true;
}

bool UE::UAI::DomainAdapters::CaptureMediaLongJob::FViewportCapture::CaptureBuffer(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError)
{
	TArray<TSharedPtr<FJsonValue>> OutArtifacts;
	if (!OutData.IsValid())
	{
		OutData = MakeShared<FJsonObject>();
	}

	FString SelectionReason;
	FLevelEditorViewportClient* LVC = UE::UAI::DomainAdapters::CaptureMediaLongJob::WorldViewportMechanics::GetPreferredLevelViewportClient(&SelectionReason);
	if (!LVC || !LVC->Viewport)
	{
		OutError = Native(TEXT("no_level_editor_viewport"), TEXT("No level editor viewport is available for buffer capture."),
			TEXT("capture_native"), TEXT("viewport_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
		OutError.InternalContext = SelectionReason;
		FUeAgentInterfaceLogger::Log(TEXT("CmdScreenshotViewportBuffer req=%s failed: %s"), *Ctx.RequestId, *OutError.ToPublicDetail());
		return false;
	}

	OutData->SetStringField(TEXT("capture_mode"), TEXT("scene_capture_buffer"));
	OutData->SetBoolField(TEXT("scene_capture_buffer"), true);
	OutData->SetBoolField(TEXT("final_lit_composition"), false);
	OutData->SetBoolField(TEXT("includes_lit_reflections"), false);
	OutData->SetStringField(TEXT("validation_scope"), TEXT("debug_buffer_only"));
	OutData->SetObjectField(TEXT("viewport"), UE::UAI::DomainAdapters::CaptureMediaLongJob::WorldViewportMechanics::MakeViewportClientJson(LVC, SelectionReason));

	FString Buffer = TEXT("SceneDepth");
	JsonTryGetString(Ctx.Params, TEXT("buffer"), Buffer);
	Buffer.TrimStartAndEndInline();
	if (Buffer.IsEmpty())
	{
		OutError = Input(TEXT("missing_buffer"), TEXT("The requested capture buffer name is empty."),
			TEXT("capture_prepare"), TEXT("/buffer"), MakeShared<FJsonValueString>(Buffer), MakeShared<FJsonValueString>(TEXT("nonempty capture buffer name")));
		return false;
	}

	const FString BufferLower = Buffer.ToLower();
	if (BufferLower == TEXT("scene_depth") || BufferLower == TEXT("scene-depth") || BufferLower == TEXT("scenedepth") || BufferLower == TEXT("depth"))
	{
		Buffer = TEXT("SceneDepth");
	}

	FString Format = TEXT("jpg");
	int32 Quality = 85;
	int32 MaxSize = 0;
	const bool bHasExplicitFormat = JsonTryGetString(Ctx.Params, TEXT("format"), Format);
	double QD = (double)Quality;
	if (JsonTryGetNumber(Ctx.Params, TEXT("quality"), QD))
	{
		Quality = FMath::Clamp((int32)QD, 1, 100);
	}
	double MSD = (double)MaxSize;
	if (JsonTryGetNumber(Ctx.Params, TEXT("max_size"), MSD))
	{
		MaxSize = FMath::Max((int32)MSD, 1);
	}

	Format = Format.ToLower();
	if (Format == TEXT("jpeg"))
	{
		Format = TEXT("jpg");
	}

	FString OutputFile;
	FString ScreenshotOutPath;
	if (JsonTryGetString(Ctx.Params, TEXT("output_file"), OutputFile) && !OutputFile.TrimStartAndEnd().IsEmpty())
	{
		ScreenshotOutPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(OutputFile);
		FString OutputExt = FPaths::GetExtension(ScreenshotOutPath).ToLower();
		if (OutputExt == TEXT("jpeg"))
		{
			OutputExt = TEXT("jpg");
		}
		if (OutputExt == TEXT("png") || OutputExt == TEXT("jpg") || OutputExt == TEXT("webp"))
		{
			if (!bHasExplicitFormat)
			{
				Format = OutputExt;
			}
			else if (OutputExt != Format)
			{
				OutError = Input(TEXT("output_file_extension_mismatch"), TEXT("The output file extension does not match the requested image format."),
					TEXT("capture_prepare"), TEXT("/output_file"), MakeShared<FJsonValueString>(OutputExt), MakeShared<FJsonValueString>(Format));
				FUeAgentInterfaceLogger::Log(TEXT("CmdScreenshotViewportBuffer req=%s failed: %s output=%s format=%s"), *Ctx.RequestId, *OutError.ToPublicDetail(), *ScreenshotOutPath, *Format);
				return false;
			}
		}
		else if (OutputExt.IsEmpty())
		{
			ScreenshotOutPath += FString::Printf(TEXT(".%s"), *Format);
		}
		else
		{
			OutError = Input(TEXT("unsupported_output_file_extension"), TEXT("The output file extension is unsupported for viewport capture."),
				TEXT("capture_prepare"), TEXT("/output_file"), MakeShared<FJsonValueString>(OutputExt), MakeShared<FJsonValueString>(TEXT("png, jpg/jpeg, or webp")));
			FUeAgentInterfaceLogger::Log(TEXT("CmdScreenshotViewportBuffer req=%s failed: %s output=%s"), *Ctx.RequestId, *OutError.ToPublicDetail(), *ScreenshotOutPath);
			return false;
		}
	}
	else
	{
		ScreenshotOutPath = MakeShotPath(Format);
	}

	if (Format != TEXT("png") && Format != TEXT("jpg") && Format != TEXT("webp"))
	{
		OutError = Input(TEXT("unsupported_format"), TEXT("The image format is unsupported for viewport capture."),
			TEXT("capture_prepare"), TEXT("/format"), MakeShared<FJsonValueString>(Format), MakeShared<FJsonValueString>(TEXT("png, jpg/jpeg, or webp")));
		FUeAgentInterfaceLogger::Log(TEXT("CmdScreenshotViewportBuffer req=%s failed: %s format=%s"), *Ctx.RequestId, *OutError.ToPublicDetail(), *Format);
		return false;
	}

	// Prefer a capture-path that doesn't rely on editor debug viewmode shaders, so we can always produce a usable depth/normal/basecolor buffer.
	enum class ECaptureBufferKind : uint8
	{
		None,
		SceneDepth,
		DeviceDepth,
		BaseColor,
		WorldNormal,
	};

	ECaptureBufferKind CaptureKind = ECaptureBufferKind::None;
	ESceneCaptureSource CaptureSource = SCS_MAX;

	const FString BufferLower2 = Buffer.ToLower();
	if (BufferLower2 == TEXT("scene_depth") || BufferLower2 == TEXT("scene-depth") || BufferLower2 == TEXT("scenedepth") || BufferLower2 == TEXT("scenedepthworldunits") || BufferLower2 == TEXT("scene_depth_world_units") || BufferLower2 == TEXT("scene-depth-world-units") || BufferLower2 == TEXT("depth"))
	{
		CaptureKind = ECaptureBufferKind::SceneDepth;
		CaptureSource = SCS_SceneDepth;
		Buffer = TEXT("SceneDepth");
	}
	else if (BufferLower2 == TEXT("device_depth") || BufferLower2 == TEXT("device-depth") || BufferLower2 == TEXT("devicedepth"))
	{
		CaptureKind = ECaptureBufferKind::DeviceDepth;
		CaptureSource = SCS_DeviceDepth;
		Buffer = TEXT("DeviceDepth");
	}
	else if (BufferLower2 == TEXT("basecolor") || BufferLower2 == TEXT("base_color") || BufferLower2 == TEXT("base-color"))
	{
		CaptureKind = ECaptureBufferKind::BaseColor;
		CaptureSource = SCS_BaseColor;
		Buffer = TEXT("BaseColor");
	}
	else if (BufferLower2 == TEXT("worldnormal") || BufferLower2 == TEXT("world_normal") || BufferLower2 == TEXT("world-normal") || BufferLower2 == TEXT("normal"))
	{
		CaptureKind = ECaptureBufferKind::WorldNormal;
		CaptureSource = SCS_Normal;
		Buffer = TEXT("WorldNormal");
	}

	if (!UE::UAI::DomainAdapters::CaptureMediaLongJob::WorldViewportMechanics::PrepareLevelViewportForCapture(LVC, OutError)) return false;

	auto TryCaptureViaSceneCapture = [&](TArray<FColor>& OutPixels, FIntPoint& OutSize) -> bool
	{
		if (CaptureKind == ECaptureBufferKind::None || CaptureSource == SCS_MAX)
		{
			OutError = Input(TEXT("unsupported_buffer"), TEXT("The requested viewport buffer is unsupported."),
				TEXT("capture_prepare"), TEXT("/buffer"), MakeShared<FJsonValueString>(Buffer), MakeShared<FJsonValueString>(TEXT("SceneDepth, DeviceDepth, BaseColor, or WorldNormal")));
			return false;
		}
		if (!GEditor)
		{
			OutError = Native(TEXT("no_geditor"), TEXT("The Editor is unavailable for scene buffer capture."),
				TEXT("capture_native"), TEXT("editor_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return false;
		}
		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!World)
		{
			OutError = Native(TEXT("no_editor_world"), TEXT("The Editor world is unavailable for scene buffer capture."),
				TEXT("capture_native"), TEXT("editor_world_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return false;
		}

		const FIntPoint ViewportSize = LVC->Viewport->GetSizeXY();
		if (ViewportSize.X <= 0 || ViewportSize.Y <= 0)
		{
			OutError = Native(TEXT("invalid_viewport_size"), TEXT("The viewport dimensions must be positive before buffer capture."),
				TEXT("capture_native"), TEXT("dimensions"), CaptureDiagnostics::Size(ViewportSize), MakeShared<FJsonValueString>(TEXT("positive width and height")));
			return false;
		}

		const int32 MaxDim = FMath::Max(ViewportSize.X, ViewportSize.Y);
		const float Scale = (MaxSize > 0 && MaxDim > MaxSize)
			? ((float)MaxSize / (float)MaxDim)
			: 1.0f;
		const int32 TargetX = FMath::Max(1, (int32)FMath::RoundToInt((float)ViewportSize.X * Scale));
		const int32 TargetY = FMath::Max(1, (int32)FMath::RoundToInt((float)ViewportSize.Y * Scale));
		OutSize = FIntPoint(TargetX, TargetY);

		UTextureRenderTarget2D* RenderTarget = NewObject<UTextureRenderTarget2D>(GetTransientPackage(), NAME_None, RF_Transient);
		if (!RenderTarget)
		{
			OutError = Native(TEXT("render_target_alloc_failed"), TEXT("A transient render target could not be allocated for scene buffer capture."),
				TEXT("capture_native"), TEXT("render_target"), Facts({{TEXT("allocated"), MakeShared<FJsonValueBoolean>(false)}, {TEXT("dimensions"), CaptureDiagnostics::Size(FIntPoint(TargetX, TargetY))}}), Facts({{TEXT("allocated"), MakeShared<FJsonValueBoolean>(true)}}));
			return false;
		}
		RenderTarget->ClearColor = FLinearColor::Black;
		RenderTarget->InitCustomFormat(TargetX, TargetY, PF_FloatRGBA, /*bForceLinearGamma*/ true);
		RenderTarget->UpdateResourceImmediate(true);

		const FVector CamLocation = LVC->GetViewLocation();
		const FRotator CamRotation = LVC->GetViewRotation();

		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		SpawnParams.ObjectFlags = RF_Transient;

		ASceneCapture2D* CaptureActor = World->SpawnActor<ASceneCapture2D>(ASceneCapture2D::StaticClass(), CamLocation, CamRotation, SpawnParams);
		if (!CaptureActor)
		{
			OutError = Native(TEXT("spawn_scene_capture_failed"), TEXT("The Editor world could not spawn a transient scene capture actor."),
				TEXT("capture_native"), TEXT("scene_capture_actor_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.TargetPath = World->GetPathName();
			return false;
		}

		struct FCaptureActorGuard
		{
			ASceneCapture2D* Actor = nullptr;
			~FCaptureActorGuard()
			{
				if (Actor && IsValid(Actor) && !Actor->IsActorBeingDestroyed())
				{
					Actor->Destroy();
				}
			}
		};

		FCaptureActorGuard Guard;
		Guard.Actor = CaptureActor;

		CaptureActor->SetFlags(RF_Transient);
		CaptureActor->SetActorHiddenInGame(true);
		CaptureActor->SetIsTemporarilyHiddenInEditor(true);

		USceneCaptureComponent2D* CaptureComp = CaptureActor->GetCaptureComponent2D();
		if (!CaptureComp)
		{
			OutError = Native(TEXT("scene_capture_component_missing"), TEXT("The scene capture actor has no capture component."),
				TEXT("capture_native"), TEXT("capture_component_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.TargetPath = CaptureActor->GetPathName();
			return false;
		}

		CaptureComp->TextureTarget = RenderTarget;
		CaptureComp->CaptureSource = CaptureSource;
		CaptureComp->bCaptureEveryFrame = false;
		CaptureComp->bCaptureOnMovement = false;
		CaptureComp->FOVAngle = LVC->ViewFOV;
		CaptureComp->CaptureScene();
		FlushRenderingCommands();

		FRenderTarget* Target = RenderTarget->GameThread_GetRenderTargetResource();
		if (!Target)
		{
			OutError = Native(TEXT("render_target_resource_missing"), TEXT("The scene capture render target has no game-thread render resource."),
				TEXT("capture_native"), TEXT("render_target"), Facts({{TEXT("resource_available"), MakeShared<FJsonValueBoolean>(false)}, {TEXT("dimensions"), CaptureDiagnostics::Size(FIntPoint(TargetX, TargetY))}}), Facts({{TEXT("resource_available"), MakeShared<FJsonValueBoolean>(true)}}));
			return false;
		}

		TArray<FLinearColor> LinearPixels;
		LinearPixels.AddUninitialized(TargetX * TargetY);
		if (!Target->ReadLinearColorPixels(LinearPixels))
		{
			OutError = Native(TEXT("read_render_target_failed"), TEXT("The render target failed to read linear color pixels for the requested capture buffer."),
				TEXT("capture_native"), TEXT("render_target"), Facts({{TEXT("pixels_read"), MakeShared<FJsonValueBoolean>(false)}, {TEXT("buffer"), MakeShared<FJsonValueString>(Buffer)}, {TEXT("dimensions"), CaptureDiagnostics::Size(FIntPoint(TargetX, TargetY))}}), Facts({{TEXT("pixels_read"), MakeShared<FJsonValueBoolean>(true)}}));
			return false;
		}

		OutPixels.Reset();
		OutPixels.AddUninitialized(TargetX * TargetY);

		if (CaptureKind == ECaptureBufferKind::SceneDepth || CaptureKind == ECaptureBufferKind::DeviceDepth)
		{
			// NOTE:
			// - `SCS_SceneDepth` returns linear scene depth in world units (cm) in R.
			// - Using a fixed `depth_far_cm` can easily produce near-black images when the viewport camera is far away.
			//   So we default to an auto-range mapping (percentiles) unless the user explicitly provides `depth_far_cm`.
			bool bInvert = true;
			JsonTryGetBool(Ctx.Params, TEXT("invert"), bInvert);

			float DepthNearCm = 0.0f;
			float DepthFarCm = 10000.0f;
			FString DepthMode;

			double DepthNearCmD = 0.0;
			double DepthFarCmD = 0.0;
			const bool bHasDepthNear = JsonTryGetNumber(Ctx.Params, TEXT("depth_near_cm"), DepthNearCmD);
			const bool bHasDepthFar = (CaptureKind == ECaptureBufferKind::SceneDepth) ? JsonTryGetNumber(Ctx.Params, TEXT("depth_far_cm"), DepthFarCmD) : false;
			JsonTryGetString(Ctx.Params, TEXT("depth_mode"), DepthMode);
			DepthMode.TrimStartAndEndInline();
			DepthMode = DepthMode.ToLower();

			const auto ApplyFixedDepthRange = [&]()
			{
				DepthNearCm = bHasDepthNear ? FMath::Max(0.0f, (float)DepthNearCmD) : 0.0f;
				const float FixedFar = bHasDepthFar ? (float)DepthFarCmD : 10000.0f;
				DepthFarCm = FMath::Max(DepthNearCm + 1.0f, FixedFar);
				OutData->SetStringField(TEXT("debug_depth_mode"), TEXT("fixed"));
				OutData->SetNumberField(TEXT("debug_depth_near_cm"), (double)DepthNearCm);
				OutData->SetNumberField(TEXT("debug_depth_far_cm"), (double)DepthFarCm);
			};

			const auto TryApplyAutoDepthRange = [&]() -> bool
			{
				// DeviceDepth is already 0..1; auto-range only makes sense for SceneDepth (cm).
				if (CaptureKind != ECaptureBufferKind::SceneDepth)
				{
					return false;
				}

				double PctLowD = 2.0;
				double PctHighD = 98.0;
				JsonTryGetNumber(Ctx.Params, TEXT("depth_auto_pct_low"), PctLowD);
				JsonTryGetNumber(Ctx.Params, TEXT("depth_auto_pct_high"), PctHighD);
				const float PctLow = FMath::Clamp((float)PctLowD, 0.0f, 49.0f);
				const float PctHigh = FMath::Clamp((float)PctHighD, 51.0f, 100.0f);

				const int32 NumPixels = LinearPixels.Num();

				TArray<float> Samples;
				Samples.Reserve(NumPixels);

				int32 SkippedSaturated = 0;
				for (int32 i = 0; i < NumPixels; ++i)
				{
					const float D = LinearPixels[i].R;
					if (!FMath::IsFinite(D) || D <= 0.0f)
					{
						continue;
					}
					// `PF_FloatRGBA` is typically half-float; `SCS_SceneDepth` can saturate at ~65504cm in large/empty views.
					// Treat saturated values as "invalid/sky/far" so auto-range is driven by real geometry depths.
					if (D >= 65000.0f)
					{
						SkippedSaturated++;
						continue;
					}
					// Filter out extreme outliers (e.g., sky/invalid depth) to keep auto-range useful.
					if (D > 10000000.0f) // 100 km in cm
					{
						continue;
					}
					Samples.Add(D);
				}

				if (Samples.IsEmpty())
				{
					return false;
				}

				Samples.Sort();
				const int32 Last = Samples.Num() - 1;
				const int32 IdxLow = FMath::Clamp((int32)FMath::RoundToInt((PctLow / 100.0f) * (float)Last), 0, Last);
				const int32 IdxHigh = FMath::Clamp((int32)FMath::RoundToInt((PctHigh / 100.0f) * (float)Last), 0, Last);

				DepthNearCm = bHasDepthNear ? FMath::Max(0.0f, (float)DepthNearCmD) : Samples[IdxLow];
				DepthFarCm = Samples[IdxHigh];
				DepthFarCm = FMath::Max(DepthNearCm + 1.0f, DepthFarCm);

				OutData->SetStringField(TEXT("debug_depth_mode"), TEXT("auto_percentile"));
				OutData->SetNumberField(TEXT("debug_depth_near_cm"), (double)DepthNearCm);
				OutData->SetNumberField(TEXT("debug_depth_far_cm"), (double)DepthFarCm);
				OutData->SetNumberField(TEXT("debug_depth_samples"), (double)Samples.Num());
				OutData->SetNumberField(TEXT("debug_depth_auto_skipped_saturated"), (double)SkippedSaturated);
				OutData->SetNumberField(TEXT("debug_depth_auto_pct_low"), (double)PctLow);
				OutData->SetNumberField(TEXT("debug_depth_auto_pct_high"), (double)PctHigh);
				return true;
			};

			// Decide mapping mode:
			// - If user provides `depth_far_cm`, keep the old fixed-far behavior (reproducible).
			// - Otherwise default to auto-percentile unless explicitly requested `fixed`.
			bool bAuto = (CaptureKind == ECaptureBufferKind::SceneDepth) && !bHasDepthFar;
			if (DepthMode == TEXT("fixed") || DepthMode == TEXT("fixed_far") || DepthMode == TEXT("far"))
			{
				bAuto = false;
			}
			else if (DepthMode == TEXT("auto") || DepthMode == TEXT("auto_percentile") || DepthMode == TEXT("percentile"))
			{
				bAuto = true;
			}

			if (!bAuto || !TryApplyAutoDepthRange())
			{
				ApplyFixedDepthRange();
			}

			uint8 MinG = 255;
			uint8 MaxG = 0;
			int32 CountNearBlack = 0;
			int32 CountNearWhite = 0;
			const int32 NumOutPixels = LinearPixels.Num();

			for (int32 i = 0; i < LinearPixels.Num(); ++i)
			{
				const FLinearColor P = LinearPixels[i];
				const float DepthValue = (CaptureKind == ECaptureBufferKind::SceneDepth) ? P.R : P.R; // DeviceDepth provides depth in RGB; use R.
				float T = 0.0f;
				if (CaptureKind == ECaptureBufferKind::DeviceDepth)
				{
					T = FMath::Clamp(DepthValue, 0.0f, 1.0f);
				}
				else
				{
					const float Range = FMath::Max(1.0f, DepthFarCm - DepthNearCm);
					T = FMath::Clamp((DepthValue - DepthNearCm) / Range, 0.0f, 1.0f);
				}
				if (bInvert)
				{
					T = 1.0f - T;
				}
				const uint8 G = (uint8)FMath::Clamp((int32)FMath::RoundToInt(T * 255.0f), 0, 255);
				OutPixels[i] = FColor(G, G, G, 255);
				MinG = FMath::Min(MinG, G);
				MaxG = FMath::Max(MaxG, G);
				if (G <= 1)
				{
					CountNearBlack++;
				}
				else if (G >= 254)
				{
					CountNearWhite++;
				}
			}

			OutData->SetNumberField(TEXT("debug_depth_min_u8"), (double)MinG);
			OutData->SetNumberField(TEXT("debug_depth_max_u8"), (double)MaxG);
			OutData->SetNumberField(TEXT("debug_depth_pct_near_black"), NumOutPixels > 0 ? ((double)CountNearBlack / (double)NumOutPixels) : 1.0);
			OutData->SetNumberField(TEXT("debug_depth_pct_near_white"), NumOutPixels > 0 ? ((double)CountNearWhite / (double)NumOutPixels) : 0.0);
			if ((int32)MaxG - (int32)MinG <= 2)
			{
				OutData->SetBoolField(TEXT("warning_low_contrast"), true);
			}
			if (NumOutPixels > 0 && (double)CountNearBlack / (double)NumOutPixels >= 0.98)
			{
				OutData->SetStringField(TEXT("warning_depth_distribution"), TEXT("mostly_black"));
			}
			else if (NumOutPixels > 0 && (double)CountNearWhite / (double)NumOutPixels >= 0.98)
			{
				OutData->SetStringField(TEXT("warning_depth_distribution"), TEXT("mostly_white"));
			}
		}
		else
		{
			for (int32 i = 0; i < LinearPixels.Num(); ++i)
			{
				OutPixels[i] = LinearPixels[i].ToFColorSRGB();
			}
		}

		return true;
	};

	if (CaptureKind != ECaptureBufferKind::None)
	{
		FUeAgentInterfaceLogger::Log(TEXT("CmdScreenshotViewportBuffer req=%s begin(scene_capture) buffer=%s format=%s quality=%d max_size=%d"),
			*Ctx.RequestId,
			*Buffer,
			*Format,
			Quality,
			MaxSize);

		TArray<FColor> Captured;
		FIntPoint CapturedSize(0, 0);
		if (!TryCaptureViaSceneCapture(Captured, CapturedSize))
		{
			FUeAgentInterfaceLogger::Log(TEXT("CmdScreenshotViewportBuffer req=%s failed(scene_capture): %s"), *Ctx.RequestId, *OutError.ToPublicDetail());
			return false;
		}

		const FString OutPath = ScreenshotOutPath;
		if (!UE::UAI::DomainAdapters::CaptureMediaLongJob::FCaptureImageProcessing::WriteCompressedImage(Captured, CapturedSize, Format, Quality, OutPath, OutError))
		{
			FUeAgentInterfaceLogger::Log(TEXT("CmdScreenshotViewportBuffer req=%s failed: %s"), *Ctx.RequestId, *OutError.ToPublicDetail());
			return false;
		}

		const int64 Bytes = IFileManager::Get().FileSize(*OutPath);
		OutData->SetStringField(TEXT("file_path"), OutPath);
		OutData->SetNumberField(TEXT("width"), CapturedSize.X);
		OutData->SetNumberField(TEXT("height"), CapturedSize.Y);
		OutData->SetNumberField(TEXT("bytes"), (double)Bytes);
		OutData->SetStringField(TEXT("format"), Format);
		OutData->SetStringField(TEXT("buffer"), Buffer);
		OutData->SetStringField(TEXT("method"), TEXT("scene_capture"));

		TSharedPtr<FJsonObject> Artifact = MakeShared<FJsonObject>();
		Artifact->SetStringField(TEXT("kind"), TEXT("screenshot"));
		Artifact->SetStringField(TEXT("file_path"), OutPath);
		Artifact->SetNumberField(TEXT("bytes"), (double)Bytes);
		Artifact->SetStringField(TEXT("mime"), Format == TEXT("png") ? TEXT("image/png") : (Format == TEXT("webp") ? TEXT("image/webp") : TEXT("image/jpeg")));
		Artifact->SetNumberField(TEXT("width"), CapturedSize.X);
		Artifact->SetNumberField(TEXT("height"), CapturedSize.Y);
		Artifact->SetStringField(TEXT("buffer"), Buffer);
		Artifact->SetStringField(TEXT("capture_mode"), TEXT("scene_capture_buffer"));
		Artifact->SetBoolField(TEXT("final_lit_composition"), false);
		OutArtifacts.Add(MakeShared<FJsonValueObject>(Artifact));

		FUeAgentInterfaceLogger::Log(TEXT("CmdScreenshotViewportBuffer req=%s ok(scene_capture) path=%s buffer=%s size=%dx%d bytes=%lld"),
			*Ctx.RequestId,
			*OutPath,
			*Buffer,
			CapturedSize.X,
			CapturedSize.Y,
			(long long)Bytes);

		return true;
	}

	const EViewModeIndex PrevViewMode = LVC->GetViewMode();
	const FName PrevBufferMode = LVC->CurrentBufferVisualizationMode;

	struct FRestoreViewportDebugView
	{
		FLevelEditorViewportClient* Client = nullptr;
		EViewModeIndex PreviousViewMode = VMI_Lit;
		FName PreviousBufferMode = NAME_None;

		~FRestoreViewportDebugView()
		{
			if (Client)
			{
				// NOTE: `ChangeBufferVisualizationMode` will force `VMI_VisualizeBuffer`.
				// When restoring a non-buffer view mode (Lit/Unlit/Wireframe/...), we must not re-enter buffer mode.
				if (PreviousViewMode == VMI_VisualizeBuffer)
				{
					Client->ChangeBufferVisualizationMode(PreviousBufferMode);
				}
				else
				{
					Client->SetViewMode(PreviousViewMode);
					Client->CurrentBufferVisualizationMode = PreviousBufferMode;
				}
				Client->Invalidate();
				if (Client->Viewport)
				{
					Client->Viewport->Invalidate();
				}
			}
		}
	};

	FRestoreViewportDebugView Restore;
	Restore.Client = LVC;
	Restore.PreviousViewMode = PrevViewMode;
	Restore.PreviousBufferMode = PrevBufferMode;

	LVC->SetViewMode(VMI_VisualizeBuffer);
	LVC->ChangeBufferVisualizationMode(FName(*Buffer));

	OutData->SetStringField(TEXT("debug_prev_view_mode"), FString::Printf(TEXT("%d"), (int32)PrevViewMode));
	OutData->SetStringField(TEXT("debug_prev_buffer_mode"), PrevBufferMode.ToString());
	OutData->SetStringField(TEXT("debug_applied_view_mode"), FString::Printf(TEXT("%d"), (int32)LVC->GetViewMode()));
	OutData->SetStringField(TEXT("debug_applied_buffer_mode"), LVC->CurrentBufferVisualizationMode.ToString());
	LVC->Invalidate();
	LVC->Viewport->Invalidate();

	FUeAgentInterfaceLogger::Log(TEXT("CmdScreenshotViewportBuffer req=%s begin buffer=%s format=%s quality=%d max_size=%d"),
		*Ctx.RequestId,
		*Buffer,
		*Format,
		Quality,
		MaxSize);

	TArray<FColor> Pixels;
	FIntPoint Size(0, 0);
	{
		FViewport* Viewport = LVC->Viewport;
		const FText ScreenshotRealtimeOverrideSystem = NSLOCTEXT("UeAgentInterface", "ScreenshotRealtimeOverrideSystem", "UeAgentInterfaceScreenshot");
		const bool bNeedTemporaryRealtime = !LVC->IsRealtime();
		if (bNeedTemporaryRealtime)
		{
			if (LVC->HasRealtimeOverride(ScreenshotRealtimeOverrideSystem))
			{
				LVC->RemoveRealtimeOverride(ScreenshotRealtimeOverrideSystem, false);
			}
			LVC->AddRealtimeOverride(true, ScreenshotRealtimeOverrideSystem);
		}

		Viewport->Invalidate();
		Viewport->Draw(/*bShouldPresent*/ true);
		FlushRenderingCommands();

		if (bNeedTemporaryRealtime && LVC->HasRealtimeOverride(ScreenshotRealtimeOverrideSystem))
		{
			LVC->RemoveRealtimeOverride(ScreenshotRealtimeOverrideSystem, false);
		}

		const FIntPoint LocalSize = Viewport->GetSizeXY();
		if (LocalSize.X <= 0 || LocalSize.Y <= 0)
		{
			OutError = Native(TEXT("invalid_viewport_size"), TEXT("The viewport dimensions must be positive before pixels can be captured."),
				TEXT("capture_native"), TEXT("dimensions"), CaptureDiagnostics::Size(LocalSize), MakeShared<FJsonValueString>(TEXT("positive width and height")));
			FUeAgentInterfaceLogger::Log(TEXT("CmdScreenshotViewportBuffer req=%s failed: %s"), *Ctx.RequestId, *OutError.ToPublicDetail());
			return false;
		}

		Pixels.AddUninitialized(LocalSize.X * LocalSize.Y);
		if (!Viewport->ReadPixels(Pixels))
		{
			OutError = Native(TEXT("read_pixels_failed"), TEXT("The viewport could not read its backbuffer pixels."),
				TEXT("capture_native"), TEXT("viewport"), Facts({{TEXT("pixels_read"), MakeShared<FJsonValueBoolean>(false)}, {TEXT("dimensions"), CaptureDiagnostics::Size(LocalSize)}}), Facts({{TEXT("pixels_read"), MakeShared<FJsonValueBoolean>(true)}}));
			FUeAgentInterfaceLogger::Log(TEXT("CmdScreenshotViewportBuffer req=%s failed: %s"), *Ctx.RequestId, *OutError.ToPublicDetail());
			return false;
		}

		Size = LocalSize;
	}

	TArray<FColor> Resized;
	FIntPoint ResizedSize(0, 0);
	if (!UE::UAI::DomainAdapters::CaptureMediaLongJob::FCaptureImageProcessing::ResizePixelsMaxSize(Pixels, Size, MaxSize, Resized, ResizedSize, OutError))
	{
		FUeAgentInterfaceLogger::Log(TEXT("CmdScreenshotViewportBuffer req=%s failed: %s"), *Ctx.RequestId, *OutError.ToPublicDetail());
		return false;
	}

	const FString OutPath = ScreenshotOutPath;
	if (!UE::UAI::DomainAdapters::CaptureMediaLongJob::FCaptureImageProcessing::WriteCompressedImage(Resized, ResizedSize, Format, Quality, OutPath, OutError))
	{
		FUeAgentInterfaceLogger::Log(TEXT("CmdScreenshotViewportBuffer req=%s failed: %s"), *Ctx.RequestId, *OutError.ToPublicDetail());
		return false;
	}

	const int64 Bytes = IFileManager::Get().FileSize(*OutPath);

	OutData->SetStringField(TEXT("file_path"), OutPath);
	OutData->SetNumberField(TEXT("width"), ResizedSize.X);
	OutData->SetNumberField(TEXT("height"), ResizedSize.Y);
	OutData->SetNumberField(TEXT("bytes"), (double)Bytes);
	OutData->SetStringField(TEXT("format"), Format);
	OutData->SetStringField(TEXT("buffer"), Buffer);
	OutData->SetStringField(TEXT("method"), TEXT("buffer_visualization"));

	TSharedPtr<FJsonObject> Artifact = MakeShared<FJsonObject>();
	Artifact->SetStringField(TEXT("kind"), TEXT("screenshot"));
	Artifact->SetStringField(TEXT("file_path"), OutPath);
	Artifact->SetNumberField(TEXT("bytes"), (double)Bytes);
	Artifact->SetStringField(TEXT("mime"), Format == TEXT("png") ? TEXT("image/png") : (Format == TEXT("webp") ? TEXT("image/webp") : TEXT("image/jpeg")));
	Artifact->SetNumberField(TEXT("width"), ResizedSize.X);
	Artifact->SetNumberField(TEXT("height"), ResizedSize.Y);
	Artifact->SetStringField(TEXT("buffer"), Buffer);
	Artifact->SetStringField(TEXT("capture_mode"), TEXT("scene_capture_buffer"));
	Artifact->SetBoolField(TEXT("final_lit_composition"), false);
	OutArtifacts.Add(MakeShared<FJsonValueObject>(Artifact));

	FUeAgentInterfaceLogger::Log(TEXT("CmdScreenshotViewportBuffer req=%s ok path=%s buffer=%s size=%dx%d bytes=%lld"),
		*Ctx.RequestId,
		*OutPath,
		*Buffer,
		ResizedSize.X,
		ResizedSize.Y,
		(long long)Bytes);

	return true;
}
