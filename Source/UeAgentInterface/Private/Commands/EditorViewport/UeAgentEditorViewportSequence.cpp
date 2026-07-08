// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commands/EditorViewport/UeAgentEditorViewportSequence.h"

#include "Utilities/UeAgentJsonDiagnostics.h"
#include "UeAgentInterfaceSettings.h"
#include "Server/UeAgentHttpServer.h"

#include "Camera/CameraComponent.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "ILevelSequenceEditorToolkit.h"
#include "ISequencer.h"
#include "LevelEditorViewport.h"
#include "LevelSequence.h"
#include "Misc/App.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "MovieScene.h"
#include "RenderingThread.h"
#include "ShowFlags.h"
#include "Slate/SceneViewport.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/AssetEditorToolkit.h"

namespace UeAgentEditorViewport
{
	static bool TryGetObjectField(const TSharedPtr<FJsonObject>& Obj, const FString& Key, TSharedPtr<FJsonObject>& OutObj)
	{
		const TSharedPtr<FJsonObject>* Found = nullptr;
		if (Obj.IsValid() && Obj->TryGetObjectField(Key, Found) && Found && Found->IsValid())
		{
			OutObj = *Found;
			return true;
		}
		return false;
	}

	static bool TryGetStringField(const TSharedPtr<FJsonObject>& Obj, const FString& Key, FString& OutValue)
	{
		return Obj.IsValid() && Obj->TryGetStringField(Key, OutValue);
	}

	static bool TryGetNumberField(const TSharedPtr<FJsonObject>& Obj, const FString& Key, double& OutValue)
	{
		return UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { Key }, OutValue);
	}

	static bool TryGetBoolField(const TSharedPtr<FJsonObject>& Obj, const FString& Key, bool& OutValue)
	{
		return Obj.IsValid() && Obj->TryGetBoolField(Key, OutValue);
	}

	static bool TryReadVectorObject(const TSharedPtr<FJsonObject>& Obj, FVector& OutValue)
	{
		if (!Obj.IsValid())
		{
			return false;
		}
		double X = 0.0;
		double Y = 0.0;
		double Z = 0.0;
		if (!UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { TEXT("x"), TEXT("X") }, X)
			|| !UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { TEXT("y"), TEXT("Y") }, Y)
			|| !UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { TEXT("z"), TEXT("Z") }, Z))
		{
			return false;
		}
		OutValue = FVector(X, Y, Z);
		return true;
	}

	static bool TryReadVectorField(const TSharedPtr<FJsonObject>& Obj, const FString& Key, FVector& OutValue)
	{
		TSharedPtr<FJsonObject> VectorObj;
		if (TryGetObjectField(Obj, Key, VectorObj))
		{
			return TryReadVectorObject(VectorObj, OutValue);
		}
		const TSharedPtr<FJsonValue>* FieldValue = UeAgentJsonDiagnostics::FindFieldValue(Obj, Key);
		return FieldValue && UeAgentJsonDiagnostics::TryReadVectorValue(*FieldValue, OutValue);
	}

	static bool HasUserFields(const TSharedPtr<FJsonObject>& Obj)
	{
		return Obj.IsValid() && Obj->Values.Num() > 0;
	}

	static FString NormalizeToken(FString Value)
	{
		Value.TrimStartAndEndInline();
		Value.ToLowerInline();
		Value.ReplaceInline(TEXT("-"), TEXT("_"));
		return Value;
	}

	static FString ViewModeToString(const EViewModeIndex ViewMode)
	{
		switch (ViewMode)
		{
		case VMI_Lit: return TEXT("lit");
		case VMI_Unlit: return TEXT("unlit");
		case VMI_BrushWireframe: return TEXT("wireframe");
		case VMI_Wireframe: return TEXT("csg_wireframe");
		case VMI_Lit_Wireframe: return TEXT("lit_wireframe");
		case VMI_Lit_DetailLighting: return TEXT("detail_lighting");
		case VMI_LightingOnly: return TEXT("lighting_only");
		case VMI_ReflectionOverride: return TEXT("reflections");
		case VMI_CollisionPawn: return TEXT("player_collision");
		case VMI_CollisionVisibility: return TEXT("visibility_collision");
		default: return FString::Printf(TEXT("unknown_%d"), static_cast<int32>(ViewMode));
		}
	}

	static bool TryParseViewMode(const FString& InValue, EViewModeIndex& OutViewMode)
	{
		const FString Value = NormalizeToken(InValue);
		if (Value == TEXT("lit")) { OutViewMode = VMI_Lit; return true; }
		if (Value == TEXT("unlit")) { OutViewMode = VMI_Unlit; return true; }
		if (Value == TEXT("wireframe")) { OutViewMode = VMI_Wireframe; return true; }
		if (Value == TEXT("brush_wireframe") || Value == TEXT("csg_wireframe")) { OutViewMode = VMI_BrushWireframe; return true; }
		if (Value == TEXT("lit_wireframe")) { OutViewMode = VMI_Lit_Wireframe; return true; }
		if (Value == TEXT("detail_lighting")) { OutViewMode = VMI_Lit_DetailLighting; return true; }
		if (Value == TEXT("lighting_only")) { OutViewMode = VMI_LightingOnly; return true; }
		if (Value == TEXT("reflections") || Value == TEXT("reflection_override")) { OutViewMode = VMI_ReflectionOverride; return true; }
		if (Value == TEXT("player_collision") || Value == TEXT("pawn_collision")) { OutViewMode = VMI_CollisionPawn; return true; }
		if (Value == TEXT("visibility_collision")) { OutViewMode = VMI_CollisionVisibility; return true; }
		return false;
	}

	static bool TryReadBoolOption(const TSharedPtr<FJsonObject>& Options, const FString& Key, bool& OutValue)
	{
		if (TryGetBoolField(Options, Key, OutValue))
		{
			return true;
		}
		TSharedPtr<FJsonObject> Overlays;
		return TryGetObjectField(Options, TEXT("overlays"), Overlays) && TryGetBoolField(Overlays, Key, OutValue);
	}

	static TSharedPtr<FJsonObject> MakeBoolOptionsJson(const TMap<FString, bool>& Values)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		for (const TPair<FString, bool>& Pair : Values)
		{
			Obj->SetBoolField(Pair.Key, Pair.Value);
		}
		return Obj;
	}

	static TSharedPtr<FJsonObject> MakeLevelVisualOptionsJson(FLevelEditorViewportClient* Client)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!Client)
		{
			return Obj;
		}
		Obj->SetStringField(TEXT("view_mode"), ViewModeToString(Client->GetViewMode()));
		Obj->SetNumberField(TEXT("view_mode_value"), static_cast<int32>(Client->GetViewMode()));
		TMap<FString, bool> Overlays;
		Overlays.Add(TEXT("grid"), Client->EngineShowFlags.Grid != 0);
		Overlays.Add(TEXT("bounds"), Client->EngineShowFlags.Bounds != 0);
		Overlays.Add(TEXT("pivot"), Client->EngineShowFlags.Pivot != 0);
		Overlays.Add(TEXT("selection"), Client->EngineShowFlags.Selection != 0);
		Overlays.Add(TEXT("mode_widgets"), Client->EngineShowFlags.ModeWidgets != 0);
		Overlays.Add(TEXT("collision"), Client->EngineShowFlags.Collision != 0);
		Obj->SetObjectField(TEXT("overlays"), MakeBoolOptionsJson(Overlays));
		Obj->SetBoolField(TEXT("realtime"), Client->IsRealtime());
		Obj->SetBoolField(TEXT("game_view"), Client->IsInGameView());
		return Obj;
	}

	static TSharedPtr<FJsonObject> MakeBoundsJson(const FBox& Box)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetObjectField(TEXT("min"), MakeVectorJson(Box.Min));
		Obj->SetObjectField(TEXT("max"), MakeVectorJson(Box.Max));
		Obj->SetObjectField(TEXT("center"), MakeVectorJson(Box.GetCenter()));
		Obj->SetObjectField(TEXT("extent"), MakeVectorJson(Box.GetExtent()));
		return Obj;
	}

	static TSharedPtr<FJsonObject> MakeFrameRateJson(const FFrameRate& Rate)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("numerator"), Rate.Numerator);
		Obj->SetNumberField(TEXT("denominator"), Rate.Denominator);
		Obj->SetNumberField(TEXT("fps"), Rate.IsValid() ? Rate.AsDecimal() : 0.0);
		return Obj;
	}

	static TSharedPtr<FJsonObject> MakePixelSignalJson(const TArray<FColor>& Pixels, const FIntPoint& Size)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("width"), Size.X);
		Obj->SetNumberField(TEXT("height"), Size.Y);
		Obj->SetNumberField(TEXT("sampled_pixel_count"), Pixels.Num());

		int32 BrightPixelCount = 0;
		int32 TransparentPixelCount = 0;
		TSet<uint32> UniqueColors;
		for (const FColor& Pixel : Pixels)
		{
			if ((static_cast<int32>(Pixel.R) + static_cast<int32>(Pixel.G) + static_cast<int32>(Pixel.B)) > 12)
			{
				++BrightPixelCount;
			}
			if (Pixel.A < 250)
			{
				++TransparentPixelCount;
			}
			if (UniqueColors.Num() <= 512)
			{
				UniqueColors.Add(Pixel.ToPackedARGB());
			}
		}
		Obj->SetNumberField(TEXT("bright_pixel_count"), BrightPixelCount);
		Obj->SetNumberField(TEXT("transparent_pixel_count"), TransparentPixelCount);
		Obj->SetNumberField(TEXT("unique_color_count_capped"), UniqueColors.Num());
		Obj->SetBoolField(TEXT("nonblank"), BrightPixelCount > 0 && UniqueColors.Num() > 1);
		return Obj;
	}

	struct FResolvedScreenshotOptions
	{
		FString Format = TEXT("png");
		FString OutputPath;
		int32 Quality = 90;
		int32 MaxSize = 1600;
		int32 WarmupFrames = 2;
	};

	static bool ParseScreenshotOptions(const TSharedPtr<FJsonObject>& Params, FResolvedScreenshotOptions& OutOptions, FString& OutError)
	{
		FString Format = TEXT("png");
		TryGetStringField(Params, TEXT("format"), Format);
		Format = NormalizeToken(Format);
		if (Format == TEXT("jpeg"))
		{
			Format = TEXT("jpg");
		}
		if (Format != TEXT("png") && Format != TEXT("jpg") && Format != TEXT("webp"))
		{
			OutError = TEXT("unsupported_format");
			return false;
		}

		int32 Quality = 90;
		double QualityD = Quality;
		if (TryGetNumberField(Params, TEXT("quality"), QualityD))
		{
			Quality = FMath::Clamp(static_cast<int32>(QualityD), 1, 100);
		}
		int32 MaxSize = 1600;
		double MaxSizeD = MaxSize;
		if (TryGetNumberField(Params, TEXT("max_size"), MaxSizeD))
		{
			MaxSize = FMath::Clamp(static_cast<int32>(MaxSizeD), 64, 8192);
		}
		int32 WarmupFrames = 2;
		double WarmupD = WarmupFrames;
		if (TryGetNumberField(Params, TEXT("warmup_frames"), WarmupD))
		{
			WarmupFrames = FMath::Clamp(static_cast<int32>(WarmupD), 0, 8);
		}

		FString OutputFile;
		FString OutputPath;
		if (TryGetStringField(Params, TEXT("output_file"), OutputFile) && !OutputFile.TrimStartAndEnd().IsEmpty())
		{
			OutputPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(OutputFile);
			FString Ext = FPaths::GetExtension(OutputPath).ToLower();
			if (Ext == TEXT("jpeg"))
			{
				Ext = TEXT("jpg");
			}
			if (Ext.IsEmpty())
			{
				OutputPath += FString::Printf(TEXT(".%s"), *Format);
			}
			else if (Ext != Format)
			{
				OutError = TEXT("output_file_extension_mismatch");
				return false;
			}
		}
		else
		{
			OutputPath = FUeAgentHttpServer::MakeShotPath(Format);
		}

		OutOptions.Format = Format;
		OutOptions.OutputPath = OutputPath;
		OutOptions.Quality = Quality;
		OutOptions.MaxSize = MaxSize;
		OutOptions.WarmupFrames = WarmupFrames;
		return true;
	}

	static bool CaptureEditorViewportClientBackbuffer(
		FEditorViewportClient* Client,
		const FString& AdapterName,
		const FResolvedScreenshotOptions& Options,
		TArray<FColor>& OutPixels,
		FIntPoint& OutSize,
		TSharedPtr<FJsonObject>& OutCaptureInfo,
		FString& OutError)
	{
		if (!Client || !Client->Viewport)
		{
			OutError = TEXT("invalid_editor_viewport_client");
			return false;
		}

		const bool bRealtimeBefore = Client->IsRealtime();
		const FText ScreenshotRealtimeOverrideSystem = NSLOCTEXT("UeAgentInterface", "EditorViewportScreenshotRealtimeOverrideSystem", "UeAgentInterfaceEditorViewportScreenshot");
		const bool bNeedTemporaryRealtime = !Client->IsRealtime();
		if (bNeedTemporaryRealtime)
		{
			if (Client->HasRealtimeOverride(ScreenshotRealtimeOverrideSystem))
			{
				Client->RemoveRealtimeOverride(ScreenshotRealtimeOverrideSystem, false);
			}
			Client->AddRealtimeOverride(true, ScreenshotRealtimeOverrideSystem);
		}

		FViewport* Viewport = Client->Viewport;
		int32 DrawsPerformed = 0;
		bool bAppliedFallbackViewportSize = false;
		FIntPoint InitialViewportSize = Viewport->GetSizeXY();
		for (int32 FrameIndex = 0; FrameIndex < Options.WarmupFrames; ++FrameIndex)
		{
			Client->Invalidate();
			Viewport->Invalidate();
			Viewport->Draw(/*bShouldPresent*/ true);
			FlushRenderingCommands();
			++DrawsPerformed;
		}

		Client->Invalidate();
		Viewport->Invalidate();
		Viewport->Draw(/*bShouldPresent*/ true);
		FlushRenderingCommands();
		++DrawsPerformed;

		if (FApp::CanEverRender() && (Viewport->GetSizeXY().X <= 0 || Viewport->GetSizeXY().Y <= 0))
		{
			FSceneViewport* SceneViewport = static_cast<FSceneViewport*>(Viewport);
			const uint32 FallbackSize = static_cast<uint32>(FMath::Clamp(Options.MaxSize, 512, 2048));
			SceneViewport->SetFixedViewportSize(FallbackSize, FallbackSize);
			Client->Invalidate();
			Viewport->Invalidate();
			Viewport->Draw(/*bShouldPresent*/ true);
			FlushRenderingCommands();
			++DrawsPerformed;
			bAppliedFallbackViewportSize = Viewport->GetSizeXY().X > 0 && Viewport->GetSizeXY().Y > 0;
		}

		if (bNeedTemporaryRealtime && Client->HasRealtimeOverride(ScreenshotRealtimeOverrideSystem))
		{
			Client->RemoveRealtimeOverride(ScreenshotRealtimeOverrideSystem, false);
		}

		const FIntPoint Size = Viewport->GetSizeXY();
		if (Size.X <= 0 || Size.Y <= 0)
		{
			if (bAppliedFallbackViewportSize)
			{
				static_cast<FSceneViewport*>(Viewport)->SetFixedViewportSize(0, 0);
			}
			OutError = TEXT("invalid_viewport_size");
			return false;
		}

		TArray<FColor> Pixels;
		Pixels.AddUninitialized(Size.X * Size.Y);
		if (!Viewport->ReadPixels(Pixels))
		{
			if (bAppliedFallbackViewportSize)
			{
				static_cast<FSceneViewport*>(Viewport)->SetFixedViewportSize(0, 0);
			}
			OutError = TEXT("read_pixels_failed");
			return false;
		}

		OutPixels = MoveTemp(Pixels);
		OutSize = Size;
		if (bAppliedFallbackViewportSize)
		{
			static_cast<FSceneViewport*>(Viewport)->SetFixedViewportSize(0, 0);
		}

		OutCaptureInfo = MakeShared<FJsonObject>();
		OutCaptureInfo->SetStringField(TEXT("schema"), TEXT("uai.viewport_capture.v1"));
		OutCaptureInfo->SetStringField(TEXT("render_path"), TEXT("editor_viewport_client_backbuffer"));
		OutCaptureInfo->SetStringField(TEXT("adapter"), AdapterName);
		TSharedPtr<FJsonObject> InitialSizeJson = MakeShared<FJsonObject>();
		InitialSizeJson->SetNumberField(TEXT("x"), InitialViewportSize.X);
		InitialSizeJson->SetNumberField(TEXT("y"), InitialViewportSize.Y);
		OutCaptureInfo->SetObjectField(TEXT("initial_viewport_size"), InitialSizeJson);
		OutCaptureInfo->SetNumberField(TEXT("warmup_frames_requested"), Options.WarmupFrames);
		OutCaptureInfo->SetNumberField(TEXT("draws_performed"), DrawsPerformed);
		OutCaptureInfo->SetBoolField(TEXT("fallback_viewport_size_applied"), bAppliedFallbackViewportSize);
		OutCaptureInfo->SetBoolField(TEXT("temporary_realtime_override"), bNeedTemporaryRealtime);
		OutCaptureInfo->SetBoolField(TEXT("realtime_before_capture"), bRealtimeBefore);
		OutCaptureInfo->SetStringField(TEXT("view_mode_name"), ViewModeToString(Client->GetViewMode()));
		OutCaptureInfo->SetNumberField(TEXT("view_mode_value"), static_cast<int32>(Client->GetViewMode()));
		return true;
	}

	static bool IsLevelSequenceAssetType(const FString& AssetType)
	{
		const FString Type = NormalizeToken(AssetType);
		return Type.IsEmpty()
			|| Type == TEXT("level_sequence")
			|| Type == TEXT("levelsequence")
			|| Type == TEXT("ulevelsequence");
	}

	static ULevelSequence* LoadLevelSequenceAsset(FString AssetPath)
	{
		AssetPath.TrimStartAndEndInline();
		if (AssetPath.IsEmpty())
		{
			return nullptr;
		}
		if (ULevelSequence* Sequence = LoadObject<ULevelSequence>(nullptr, *AssetPath))
		{
			return Sequence;
		}
		if (FPackageName::IsValidLongPackageName(AssetPath))
		{
			const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
			const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *AssetPath, *AssetName);
			return LoadObject<ULevelSequence>(nullptr, *ObjectPath);
		}
		return nullptr;
	}

	static ILevelSequenceEditorToolkit* ResolveLevelSequenceEditor(ULevelSequence* Sequence, const bool bOpenEditorIfNeeded)
	{
		if (!GEditor || !Sequence)
		{
			return nullptr;
		}

		UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		if (!AssetEditorSubsystem)
		{
			return nullptr;
		}

		auto GetOpenedToolkit = [AssetEditorSubsystem, Sequence]() -> ILevelSequenceEditorToolkit*
		{
			IAssetEditorInstance* EditorInstance = AssetEditorSubsystem->FindEditorForAsset(Sequence, false);
			if (!EditorInstance || EditorInstance->GetEditorName() != FName(TEXT("LevelSequenceEditor")))
			{
				return nullptr;
			}
			FAssetEditorToolkit* Toolkit = static_cast<FAssetEditorToolkit*>(EditorInstance);
			return static_cast<ILevelSequenceEditorToolkit*>(Toolkit);
		};

		if (ILevelSequenceEditorToolkit* Existing = GetOpenedToolkit())
		{
			return Existing;
		}

		if (!bOpenEditorIfNeeded)
		{
			return nullptr;
		}

		AssetEditorSubsystem->OpenEditorForAsset(Sequence);
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().Tick();
		}
		return GetOpenedToolkit();
	}

	static FLevelEditorViewportClient* GetPreferredLevelViewportClient(FString* OutSelectionReason = nullptr)
	{
		auto SetReason = [OutSelectionReason](const TCHAR* Reason)
		{
			if (OutSelectionReason)
			{
				*OutSelectionReason = Reason;
			}
		};

		if (GEditor)
		{
			if (FViewport* ActiveViewport = GEditor->GetActiveViewport())
			{
				for (FLevelEditorViewportClient* Client : GEditor->GetLevelViewportClients())
				{
					if (Client && Client->Viewport == ActiveViewport)
					{
						SetReason(TEXT("active_viewport"));
						return Client;
					}
				}
			}

			int32 Index = 0;
			for (FLevelEditorViewportClient* Client : GEditor->GetLevelViewportClients())
			{
				if (Client && Client->Viewport)
				{
					SetReason(*FString::Printf(TEXT("level_viewport_client_%d"), Index));
					return Client;
				}
				++Index;
			}
		}

		if (GCurrentLevelEditingViewportClient && GCurrentLevelEditingViewportClient->Viewport)
		{
			SetReason(TEXT("current_level_editing_viewport"));
			return GCurrentLevelEditingViewportClient;
		}

		SetReason(TEXT("none"));
		return nullptr;
	}

	struct FSequenceTimeRequest
	{
		FFrameTime TickTime;
		double RelativeTimeSeconds = 0.0;
		int32 DisplayFrameIndex = 0;
		FString TimeSource;
		double NormalizedTime = 0.0;
		bool bHasNormalizedTime = false;
	};

	static bool TryGetSequenceNumberAnyField(const TSharedPtr<FJsonObject>& Obj, const TArray<FString>& Keys, double& OutValue)
	{
		if (!Obj.IsValid())
		{
			return false;
		}
		for (const FString& Key : Keys)
		{
			if (Obj->TryGetNumberField(Key, OutValue))
			{
				return true;
			}
		}
		return false;
	}

	static bool ResolveSequenceTimeRequest(ULevelSequence* Sequence, const TSharedPtr<FJsonObject>& PreviewState, FSequenceTimeRequest& OutRequest, FString& OutError)
	{
		if (!Sequence || !Sequence->GetMovieScene())
		{
			OutError = TEXT("movie_scene_not_found");
			return false;
		}

		UMovieScene* MovieScene = Sequence->GetMovieScene();
		const FFrameRate TickResolution = MovieScene->GetTickResolution();
		const FFrameRate DisplayRate = MovieScene->GetDisplayRate();
		const double FramesPerSecond = DisplayRate.IsValid() && DisplayRate.AsDecimal() > KINDA_SMALL_NUMBER
			? DisplayRate.AsDecimal()
			: 30.0;
		const double SecondsPerFrame = 1.0 / FramesPerSecond;

		const TRange<FFrameNumber> PlaybackRange = MovieScene->GetPlaybackRange();
		const FFrameNumber StartTick = PlaybackRange.GetLowerBoundValue();
		const FFrameNumber EndExclusiveTick = PlaybackRange.GetUpperBoundValue();
		if (EndExclusiveTick.Value <= StartTick.Value)
		{
			OutError = TEXT("invalid_sequence_playback_range");
			return false;
		}
		const double StartSeconds = FQualifiedFrameTime(FFrameTime(StartTick), TickResolution).AsSeconds();
		const double EndSeconds = FQualifiedFrameTime(FFrameTime(EndExclusiveTick), TickResolution).AsSeconds();
		const double DurationSeconds = FMath::Max(EndSeconds - StartSeconds, KINDA_SMALL_NUMBER);

		double NormalizedTime = 0.0;
		if (TryGetSequenceNumberAnyField(PreviewState, { TEXT("normalized_time") }, NormalizedTime))
		{
			if (NormalizedTime < 0.0 || NormalizedTime > 1.0)
			{
				OutError = TEXT("invalid_normalized_time");
				return false;
			}
			const double LastInclusiveTick = static_cast<double>(EndExclusiveTick.Value - 1);
			OutRequest.TickTime = FFrameTime::FromDecimal(FMath::Lerp(static_cast<double>(StartTick.Value), LastInclusiveTick, NormalizedTime));
			OutRequest.RelativeTimeSeconds = FQualifiedFrameTime(OutRequest.TickTime, TickResolution).AsSeconds() - StartSeconds;
			OutRequest.DisplayFrameIndex = FMath::RoundToInt(OutRequest.RelativeTimeSeconds / SecondsPerFrame);
			OutRequest.TimeSource = TEXT("normalized_time");
			OutRequest.NormalizedTime = NormalizedTime;
			OutRequest.bHasNormalizedTime = true;
			return true;
		}

		double TickFrameValue = 0.0;
		if (TryGetSequenceNumberAnyField(PreviewState, { TEXT("tick_frame"), TEXT("evaluation_frame") }, TickFrameValue))
		{
			OutRequest.TickTime = FFrameTime::FromDecimal(TickFrameValue);
			OutRequest.RelativeTimeSeconds = FQualifiedFrameTime(OutRequest.TickTime, TickResolution).AsSeconds() - StartSeconds;
			OutRequest.DisplayFrameIndex = FMath::RoundToInt(OutRequest.RelativeTimeSeconds / SecondsPerFrame);
			OutRequest.TimeSource = TEXT("tick_frame");
		}
		else
		{
			double TimeSeconds = 0.0;
			if (TryGetSequenceNumberAnyField(PreviewState, { TEXT("absolute_time_seconds"), TEXT("absolute_time") }, TimeSeconds))
			{
				if (TimeSeconds < -KINDA_SMALL_NUMBER)
				{
					OutError = TEXT("invalid_absolute_time_seconds");
					return false;
				}
				OutRequest.TickTime = TickResolution.AsFrameTime(TimeSeconds);
				OutRequest.RelativeTimeSeconds = TimeSeconds - StartSeconds;
				OutRequest.DisplayFrameIndex = FMath::RoundToInt(OutRequest.RelativeTimeSeconds / SecondsPerFrame);
				OutRequest.TimeSource = TEXT("absolute_time_seconds");
			}
			else if (TryGetSequenceNumberAnyField(PreviewState, { TEXT("time_seconds"), TEXT("seconds") }, TimeSeconds))
			{
				if (TimeSeconds < -KINDA_SMALL_NUMBER)
				{
					OutError = TEXT("invalid_time_seconds");
					return false;
				}
				const double AbsoluteSeconds = StartSeconds + TimeSeconds;
				OutRequest.TickTime = TickResolution.AsFrameTime(AbsoluteSeconds);
				OutRequest.RelativeTimeSeconds = TimeSeconds;
				OutRequest.DisplayFrameIndex = FMath::RoundToInt(TimeSeconds / SecondsPerFrame);
				OutRequest.TimeSource = TEXT("time_seconds");
			}
			else
			{
				double FrameValue = 0.0;
				if (!TryGetSequenceNumberAnyField(PreviewState, { TEXT("frame_index"), TEXT("frame"), TEXT("display_frame") }, FrameValue))
				{
					OutError = TEXT("missing_sequence_time");
					return false;
				}
				const int32 FrameIndex = FMath::RoundToInt(FrameValue);
				if (FrameIndex < 0)
				{
					OutError = TEXT("invalid_frame_index");
					return false;
				}
				const double RelativeSeconds = static_cast<double>(FrameIndex) * SecondsPerFrame;
				const double AbsoluteSeconds = StartSeconds + RelativeSeconds;
				OutRequest.TickTime = TickResolution.AsFrameTime(AbsoluteSeconds);
				OutRequest.RelativeTimeSeconds = RelativeSeconds;
				OutRequest.DisplayFrameIndex = FrameIndex;
				OutRequest.TimeSource = TEXT("frame_index");
			}
		}

		const FFrameNumber EvalFloor = OutRequest.TickTime.FloorToFrame();
		if (EvalFloor.Value < StartTick.Value || EvalFloor.Value >= EndExclusiveTick.Value)
		{
			bool bAllowOutOfRange = false;
			TryGetBoolField(PreviewState, TEXT("allow_out_of_range"), bAllowOutOfRange);
			if (!bAllowOutOfRange)
			{
				OutError = TEXT("sequence_time_out_of_playback_range");
				return false;
			}
		}

		OutRequest.NormalizedTime = DurationSeconds > KINDA_SMALL_NUMBER
			? FMath::Clamp(OutRequest.RelativeTimeSeconds / DurationSeconds, 0.0, 1.0)
			: 0.0;
		return true;
	}

	static TSharedPtr<FJsonObject> MakeSequencePreviewStateJson(ULevelSequence* Sequence, ISequencer* Sequencer)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("context"), TEXT("level_sequence_editor"));
		Obj->SetStringField(TEXT("camera_source"), TEXT("level_viewport"));
		Obj->SetStringField(TEXT("camera_owner"), TEXT("level_viewport"));
		Obj->SetStringField(TEXT("sequence_time_space"), TEXT("local_playback_range_relative"));
		Obj->SetStringField(TEXT("asset_path"), Sequence ? Sequence->GetPathName() : FString());
		Obj->SetStringField(TEXT("sequence_path"), Sequence ? Sequence->GetPathName() : FString());
		Obj->SetBoolField(TEXT("has_sequencer"), Sequencer != nullptr);

		if (!Sequence || !Sequence->GetMovieScene())
		{
			Obj->SetStringField(TEXT("state"), TEXT("movie_scene_not_found"));
			return Obj;
		}

		UMovieScene* MovieScene = Sequence->GetMovieScene();
		const FFrameRate TickResolution = MovieScene->GetTickResolution();
		const FFrameRate DisplayRate = MovieScene->GetDisplayRate();
		const double FramesPerSecond = DisplayRate.IsValid() && DisplayRate.AsDecimal() > KINDA_SMALL_NUMBER
			? DisplayRate.AsDecimal()
			: 30.0;
		const double SecondsPerFrame = 1.0 / FramesPerSecond;
		const TRange<FFrameNumber> PlaybackRange = MovieScene->GetPlaybackRange();
		const FFrameNumber StartTick = PlaybackRange.GetLowerBoundValue();
		const FFrameNumber EndExclusiveTick = PlaybackRange.GetUpperBoundValue();
		const double StartSeconds = FQualifiedFrameTime(FFrameTime(StartTick), TickResolution).AsSeconds();
		const double EndSeconds = FQualifiedFrameTime(FFrameTime(EndExclusiveTick), TickResolution).AsSeconds();
		const double DurationSeconds = FMath::Max(EndSeconds - StartSeconds, 0.0);

		Obj->SetObjectField(TEXT("display_rate"), MakeFrameRateJson(DisplayRate));
		Obj->SetObjectField(TEXT("tick_resolution"), MakeFrameRateJson(TickResolution));

		TSharedPtr<FJsonObject> RangeObj = MakeShared<FJsonObject>();
		RangeObj->SetNumberField(TEXT("start_tick_frame"), StartTick.Value);
		RangeObj->SetNumberField(TEXT("end_tick_frame_exclusive"), EndExclusiveTick.Value);
		RangeObj->SetNumberField(TEXT("start_seconds"), StartSeconds);
		RangeObj->SetNumberField(TEXT("duration_seconds"), DurationSeconds);
		Obj->SetObjectField(TEXT("playback_range"), RangeObj);

		if (Sequencer)
		{
			const FQualifiedFrameTime LocalTime = Sequencer->GetLocalTime();
			const FFrameTime TickTime = LocalTime.ConvertTo(TickResolution);
			const double CurrentSeconds = FQualifiedFrameTime(TickTime, TickResolution).AsSeconds();
			const double RelativeSeconds = CurrentSeconds - StartSeconds;
			const int32 FrameIndex = FMath::RoundToInt(RelativeSeconds / SecondsPerFrame);
			Obj->SetNumberField(TEXT("tick_frame"), TickTime.AsDecimal());
			Obj->SetNumberField(TEXT("time_seconds"), RelativeSeconds);
			Obj->SetNumberField(TEXT("frame_index"), FrameIndex);
			Obj->SetNumberField(TEXT("frame"), FrameIndex);
			Obj->SetNumberField(TEXT("normalized_time"), DurationSeconds > KINDA_SMALL_NUMBER ? FMath::Clamp(RelativeSeconds / DurationSeconds, 0.0, 1.0) : 0.0);
			Obj->SetObjectField(TEXT("local_time_rate"), MakeFrameRateJson(LocalTime.Rate));

			TWeakObjectPtr<UCameraComponent> CameraCut = Sequencer->GetLastEvaluatedCameraCut();
			Obj->SetBoolField(TEXT("sequence_camera_cut_available"), CameraCut.IsValid());
			if (CameraCut.IsValid())
			{
				Obj->SetStringField(TEXT("last_evaluated_camera_cut_component"), CameraCut.Get()->GetPathName());
			}
		}
		return Obj;
	}

	class FLevelSequenceViewportAdapter final : public IViewportAdapter
	{
	public:
		virtual FString GetName() const override
		{
			return TEXT("level_sequence");
		}

		virtual bool CanResolve(const FResolveRequest& Request) const override
		{
			if (Request.Target.Scope != TEXT("asset_editor"))
			{
				return false;
			}
			return IsLevelSequenceAssetType(Request.Target.AssetType);
		}

		virtual bool Resolve(const FResolveRequest& Request, FResolvedViewport& OutViewport, FString& OutError) const override
		{
			if (Request.Target.AssetPath.IsEmpty())
			{
				OutError = TEXT("missing_asset_path");
				return false;
			}

			ULevelSequence* Sequence = LoadLevelSequenceAsset(Request.Target.AssetPath);
			if (!Sequence)
			{
				OutError = TEXT("level_sequence_not_found");
				return false;
			}

			ILevelSequenceEditorToolkit* Toolkit = ResolveLevelSequenceEditor(Sequence, Request.Target.bOpenEditorIfNeeded);
			if (!Toolkit || !Toolkit->GetSequencer().IsValid())
			{
				OutError = TEXT("level_sequence_editor_not_opened");
				return false;
			}

			FString SelectionReason;
			FLevelEditorViewportClient* Client = GetPreferredLevelViewportClient(&SelectionReason);
			if (!Client)
			{
				OutError = TEXT("no_level_editor_viewport");
				return false;
			}

			OutViewport.Target = Request.Target;
			OutViewport.Target.Scope = TEXT("asset_editor");
			OutViewport.Target.AssetPath = Sequence->GetPathName();
			OutViewport.Target.AssetType = TEXT("level_sequence");
			OutViewport.Target.ViewportRole = TEXT("sequence");
			OutViewport.Target.ViewportId = FString::Printf(TEXT("asset:%s#level_sequence_%s"), *Sequence->GetPathName(), *SelectionReason);
			OutViewport.AdapterName = GetName();
			OutViewport.ClientClass = TEXT("FLevelEditorViewportClient");
			OutViewport.ViewportClient = Client;
			OutViewport.Asset = Sequence;
			return true;
		}

		virtual bool ReadState(const FResolvedViewport& Viewport, FViewportState& OutState, FString& OutError) const override
		{
			ULevelSequence* Sequence = Cast<ULevelSequence>(Viewport.Asset);
			ILevelSequenceEditorToolkit* Toolkit = ResolveLevelSequenceEditor(Sequence, false);
			TSharedPtr<ISequencer> Sequencer = Toolkit ? Toolkit->GetSequencer() : nullptr;
			FLevelEditorViewportClient* Client = AsLevelClient(Viewport);
			if (!Sequence || !Sequencer.IsValid() || !Client)
			{
				OutError = TEXT("invalid_level_sequence_viewport");
				return false;
			}

			OutState.Target = Viewport.Target;
			OutState.ClientClass = TEXT("FLevelEditorViewportClient");
			OutState.Adapter = GetName();
			OutState.Camera.Location = Client->GetViewLocation();
			OutState.Camera.Rotation = Client->GetViewRotation();
			OutState.Camera.Fov = Client->ViewFOV;
			OutState.Camera.Space = TEXT("editor_world");
			OutState.Camera.Projection = Client->ViewportType == LVT_Perspective ? TEXT("perspective") : TEXT("orthographic");
			OutState.Camera.bValid = true;
			OutState.Capabilities.bCamera = true;
			OutState.Capabilities.bVisualOptions = true;
			OutState.Capabilities.bPreviewState = true;
			OutState.Capabilities.bScreenshot = true;
			OutState.Capabilities.FocusModes = { TEXT("custom_bounds"), TEXT("custom_point") };
			OutState.Capabilities.ScreenshotBackends = { TEXT("level_sequence_level_viewport_backbuffer") };
			OutState.Capabilities.VisualOptionKeys = { TEXT("view_mode"), TEXT("grid"), TEXT("bounds"), TEXT("pivot"), TEXT("selection"), TEXT("mode_widgets"), TEXT("collision") };
			OutState.Capabilities.PreviewStateKeys = { TEXT("frame"), TEXT("frame_index"), TEXT("time_seconds"), TEXT("normalized_time"), TEXT("tick_frame"), TEXT("camera_source") };
			OutState.VisualOptions = MakeLevelVisualOptionsJson(Client);
			OutState.PreviewState = MakeSequencePreviewStateJson(Sequence, Sequencer.Get());
			return true;
		}

		virtual bool WriteCamera(const FResolvedViewport& Viewport, const FViewportCamera& Camera, FString& OutError) const override
		{
			FLevelEditorViewportClient* Client = AsLevelClient(Viewport);
			if (!Client)
			{
				OutError = TEXT("invalid_level_sequence_viewport");
				return false;
			}
			Client->SetViewLocation(Camera.Location);
			Client->SetViewRotation(Camera.Rotation);
			Client->ViewFOV = FMath::Clamp(Camera.Fov, 5.0f, 170.0f);
			Client->Invalidate();
			if (Client->Viewport)
			{
				Client->Viewport->Invalidate();
			}
			return true;
		}

		virtual bool Focus(const FResolvedViewport& Viewport, const TSharedPtr<FJsonObject>& FocusObj, TSharedPtr<FJsonObject>& OutData, FString& OutError) const override
		{
			FLevelEditorViewportClient* Client = AsLevelClient(Viewport);
			if (!Client)
			{
				OutError = TEXT("invalid_level_sequence_viewport");
				return false;
			}
			if (!FocusObj.IsValid())
			{
				OutError = TEXT("missing_focus");
				return false;
			}

			FString Mode = TEXT("custom_bounds");
			TryGetStringField(FocusObj, TEXT("mode"), Mode);
			Mode = NormalizeToken(Mode);
			OutData = MakeShared<FJsonObject>();
			OutData->SetStringField(TEXT("mode"), Mode);

			if (Mode == TEXT("custom_bounds"))
			{
				FVector Min = FVector::ZeroVector;
				FVector Max = FVector::ZeroVector;
				if (!TryReadVectorField(FocusObj, TEXT("min"), Min) || !TryReadVectorField(FocusObj, TEXT("max"), Max))
				{
					TSharedPtr<FJsonObject> BoundsObj;
					if (!TryGetObjectField(FocusObj, TEXT("bounds"), BoundsObj)
						|| !TryReadVectorField(BoundsObj, TEXT("min"), Min)
						|| !TryReadVectorField(BoundsObj, TEXT("max"), Max))
					{
						OutError = TEXT("missing_custom_bounds");
						return false;
					}
				}
				const FBox Box(Min, Max);
				if (!Box.IsValid)
				{
					OutError = TEXT("invalid_custom_bounds");
					return false;
				}
				bool bInstant = true;
				TryGetBoolField(FocusObj, TEXT("instant"), bInstant);
				Client->FocusViewportOnBox(Box, bInstant);
				Client->Invalidate();
				OutData->SetStringField(TEXT("bounds_space"), TEXT("editor_world"));
				OutData->SetObjectField(TEXT("bounds"), MakeBoundsJson(Box));
				OutData->SetBoolField(TEXT("instant"), bInstant);
				return true;
			}

			if (Mode == TEXT("custom_point"))
			{
				FVector Point = FVector::ZeroVector;
				if (!TryReadVectorField(FocusObj, TEXT("point"), Point))
				{
					OutError = TEXT("missing_custom_point");
					return false;
				}
				double Radius = 50.0;
				TryGetNumberField(FocusObj, TEXT("radius"), Radius);
				const FBox Box(Point - FVector(Radius), Point + FVector(Radius));
				Client->FocusViewportOnBox(Box, true);
				Client->Invalidate();
				OutData->SetStringField(TEXT("point_space"), TEXT("editor_world"));
				OutData->SetObjectField(TEXT("point"), MakeVectorJson(Point));
				OutData->SetNumberField(TEXT("radius"), Radius);
				return true;
			}

			OutError = TEXT("unsupported_focus_mode");
			return false;
		}

		virtual bool ApplyVisualOptions(const FResolvedViewport& Viewport, const TSharedPtr<FJsonObject>& Options, TSharedPtr<FJsonObject>& OutData, FString& OutError) const override
		{
			FLevelEditorViewportClient* Client = AsLevelClient(Viewport);
			if (!Client)
			{
				OutError = TEXT("invalid_level_sequence_viewport");
				return false;
			}
			if (!Options.IsValid())
			{
				OutError = TEXT("missing_visual_options");
				return false;
			}

			TArray<FString> Unsupported;
			FString ViewModeText;
			if (TryGetStringField(Options, TEXT("view_mode"), ViewModeText))
			{
				EViewModeIndex ViewMode = VMI_Lit;
				if (!TryParseViewMode(ViewModeText, ViewMode))
				{
					Unsupported.Add(TEXT("view_mode"));
				}
				else
				{
					Client->SetViewMode(ViewMode);
				}
			}

			auto ApplyBoolFlag = [Client, Options](const TCHAR* Key, auto Setter)
			{
				bool bValue = false;
				if (TryReadBoolOption(Options, Key, bValue))
				{
					Setter(bValue);
				}
			};
			ApplyBoolFlag(TEXT("grid"), [Client](bool bValue) { Client->EngineShowFlags.SetGrid(bValue); });
			ApplyBoolFlag(TEXT("bounds"), [Client](bool bValue) { Client->EngineShowFlags.SetBounds(bValue); });
			ApplyBoolFlag(TEXT("pivot"), [Client](bool bValue) { Client->EngineShowFlags.SetPivot(bValue); });
			ApplyBoolFlag(TEXT("selection"), [Client](bool bValue) { Client->EngineShowFlags.SetSelection(bValue); });
			ApplyBoolFlag(TEXT("mode_widgets"), [Client](bool bValue) { Client->EngineShowFlags.SetModeWidgets(bValue); });
			ApplyBoolFlag(TEXT("collision"), [Client](bool bValue) { Client->EngineShowFlags.SetCollision(bValue); });

			bool bRealtime = Client->IsRealtime();
			if (TryGetBoolField(Options, TEXT("realtime"), bRealtime))
			{
				Client->SetRealtime(bRealtime);
			}
			bool bGameView = Client->IsInGameView();
			if (TryGetBoolField(Options, TEXT("game_view"), bGameView))
			{
				Client->SetGameView(bGameView);
			}

			Client->Invalidate();
			if (Client->Viewport)
			{
				Client->Viewport->Invalidate();
			}

			OutData = MakeLevelVisualOptionsJson(Client);
			if (Unsupported.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> UnsupportedJson;
				for (const FString& Key : Unsupported)
				{
					UnsupportedJson.Add(MakeShared<FJsonValueString>(Key));
				}
				OutData->SetArrayField(TEXT("unsupported_options"), UnsupportedJson);
				bool bIgnoreUnsupported = false;
				TryGetBoolField(Options, TEXT("ignore_unsupported"), bIgnoreUnsupported);
				if (!bIgnoreUnsupported)
				{
					OutError = TEXT("unsupported_visual_options");
					return false;
				}
			}
			return true;
		}

		virtual bool ApplyPreviewState(const FResolvedViewport& Viewport, const TSharedPtr<FJsonObject>& PreviewState, TSharedPtr<FJsonObject>& OutData, FString& OutError) const override
		{
			OutData = MakeShared<FJsonObject>();
			if (!HasUserFields(PreviewState))
			{
				OutData->SetBoolField(TEXT("applied"), false);
				OutData->SetStringField(TEXT("reason"), TEXT("empty_preview_state"));
				return true;
			}

			ULevelSequence* Sequence = Cast<ULevelSequence>(Viewport.Asset);
			ILevelSequenceEditorToolkit* Toolkit = ResolveLevelSequenceEditor(Sequence, false);
			TSharedPtr<ISequencer> Sequencer = Toolkit ? Toolkit->GetSequencer() : nullptr;
			if (!Sequence || !Sequencer.IsValid())
			{
				OutError = TEXT("level_sequence_editor_not_opened");
				return false;
			}

			FSequenceTimeRequest TimeRequest;
			if (!ResolveSequenceTimeRequest(Sequence, PreviewState, TimeRequest, OutError))
			{
				return false;
			}

			Sequencer->SetLocalTime(TimeRequest.TickTime, STM_None, true);
			Sequencer->ForceEvaluate();

			FString CameraSource = TEXT("level_viewport");
			TryGetStringField(PreviewState, TEXT("camera_source"), CameraSource);
			CameraSource = NormalizeToken(CameraSource);
			bool bCameraCutApplied = false;
			if (CameraSource == TEXT("sequence_camera_cut") || CameraSource == TEXT("active_camera_cut") || CameraSource == TEXT("camera_cut"))
			{
				TWeakObjectPtr<UCameraComponent> CameraCut = Sequencer->GetLastEvaluatedCameraCut();
				if (!CameraCut.IsValid())
				{
					OutError = TEXT("sequence_camera_cut_not_available");
					return false;
				}
				FLevelEditorViewportClient* Client = AsLevelClient(Viewport);
				if (!Client)
				{
					OutError = TEXT("invalid_level_sequence_viewport");
					return false;
				}
				UCameraComponent* CameraComponent = CameraCut.Get();
				Client->SetViewLocation(CameraComponent->GetComponentLocation());
				Client->SetViewRotation(CameraComponent->GetComponentRotation());
				Client->ViewFOV = FMath::Clamp(CameraComponent->FieldOfView, 5.0f, 170.0f);
				Client->Invalidate();
				if (Client->Viewport)
				{
					Client->Viewport->Invalidate();
				}
				bCameraCutApplied = true;
				CameraSource = TEXT("sequence_camera_cut");
			}
			else if (CameraSource != TEXT("level_viewport"))
			{
				OutError = TEXT("unsupported_sequence_camera_source");
				return false;
			}

			OutData->SetBoolField(TEXT("applied"), true);
			OutData->SetStringField(TEXT("mode"), TEXT("set_local_time"));
			OutData->SetStringField(TEXT("time_source"), TimeRequest.TimeSource);
			OutData->SetNumberField(TEXT("tick_frame"), TimeRequest.TickTime.AsDecimal());
			OutData->SetNumberField(TEXT("time_seconds"), TimeRequest.RelativeTimeSeconds);
			OutData->SetNumberField(TEXT("frame_index"), TimeRequest.DisplayFrameIndex);
			OutData->SetNumberField(TEXT("frame"), TimeRequest.DisplayFrameIndex);
			OutData->SetNumberField(TEXT("normalized_time"), TimeRequest.NormalizedTime);
			OutData->SetStringField(TEXT("camera_source"), CameraSource);
			OutData->SetBoolField(TEXT("camera_cut_applied"), bCameraCutApplied);
			OutData->SetStringField(TEXT("preview_state_token"), FString::Printf(TEXT("level_sequence:%s:%0.3f"), *Sequence->GetPathName(), TimeRequest.TickTime.AsDecimal()));
			return true;
		}

		virtual bool CaptureScreenshot(const FResolvedViewport& Viewport, const TSharedPtr<FJsonObject>& Params, FViewportScreenshotResult& OutResult, FString& OutError) const override
		{
			if (!AsLevelClient(Viewport))
			{
				OutError = TEXT("invalid_level_sequence_viewport");
				return false;
			}
			const UUeAgentInterfaceSettings* Settings = UUeAgentInterfaceSettings::Get();
			if (!Settings || !Settings->bEnableScreenshots)
			{
				OutError = TEXT("screenshots_disabled");
				return false;
			}
			FResolvedScreenshotOptions Options;
			if (!ParseScreenshotOptions(Params, Options, OutError))
			{
				return false;
			}

			TArray<FColor> Pixels;
			FIntPoint Size(0, 0);
			TSharedPtr<FJsonObject> CaptureInfo;
			if (!CaptureEditorViewportClientBackbuffer(Viewport.ViewportClient, GetName(), Options, Pixels, Size, CaptureInfo, OutError))
			{
				return false;
			}

			TArray<FColor> Resized;
			FIntPoint ResizedSize(0, 0);
			if (!FUeAgentHttpServer::ResizePixelsMaxSize(Pixels, Size, Options.MaxSize, Resized, ResizedSize, OutError))
			{
				return false;
			}
			if (!FUeAgentHttpServer::WriteCompressedImage(Resized, ResizedSize, Options.Format, Options.Quality, Options.OutputPath, OutError))
			{
				return false;
			}

			OutResult.FilePath = Options.OutputPath;
			OutResult.Format = Options.Format;
			OutResult.Width = ResizedSize.X;
			OutResult.Height = ResizedSize.Y;
			OutResult.Bytes = IFileManager::Get().FileSize(*Options.OutputPath);
			OutResult.CaptureBackend = TEXT("level_sequence_level_viewport_backbuffer");
			OutResult.PixelSignal = MakePixelSignalJson(Resized, ResizedSize);
			OutResult.Capture = CaptureInfo;
			return true;
		}

		virtual bool SnapshotState(const FResolvedViewport& Viewport, FViewportSnapshot& OutSnapshot, FString& OutError) const override
		{
			OutSnapshot = FViewportSnapshot();
			if (!ReadState(Viewport, OutSnapshot.State, OutError))
			{
				return false;
			}
			OutSnapshot.bValid = true;
			return true;
		}

		virtual bool RestoreState(const FResolvedViewport& Viewport, const FViewportSnapshot& Snapshot, FString& OutError) const override
		{
			if (!Snapshot.bValid)
			{
				OutError = TEXT("invalid_snapshot");
				return false;
			}
			TSharedPtr<FJsonObject> AppliedPreview;
			if (!ApplyPreviewState(Viewport, Snapshot.State.PreviewState, AppliedPreview, OutError))
			{
				return false;
			}
			if (!WriteCamera(Viewport, Snapshot.State.Camera, OutError))
			{
				return false;
			}
			TSharedPtr<FJsonObject> AppliedVisual;
			return ApplyVisualOptions(Viewport, Snapshot.State.VisualOptions, AppliedVisual, OutError);
		}

	private:
		static FLevelEditorViewportClient* AsLevelClient(const FResolvedViewport& Viewport)
		{
			return static_cast<FLevelEditorViewportClient*>(Viewport.ViewportClient);
		}
	};

	TSharedRef<IViewportAdapter> MakeLevelSequenceViewportAdapter()
	{
		return MakeShared<FLevelSequenceViewportAdapter>();
	}
}
