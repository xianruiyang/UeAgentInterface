// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commands/EditorViewport/UeAgentHttpServer_EditorViewportInternal.h"

#include "Server/UeAgentHttpServer.h"
#include "Utilities/UeAgentJsonDiagnostics.h"

#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "LevelEditorViewport.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "RenderingThread.h"
#include "Slate/SceneViewport.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"

namespace UeAgentEditorViewport
{
	bool TryGetObjectField(const TSharedPtr<FJsonObject>& Obj, const FString& Key, TSharedPtr<FJsonObject>& OutObj)
	{
		const TSharedPtr<FJsonObject>* Found = nullptr;
		if (Obj.IsValid() && Obj->TryGetObjectField(Key, Found) && Found && Found->IsValid())
		{
			OutObj = *Found;
			return true;
		}
		return false;
	}

	bool TryGetStringField(const TSharedPtr<FJsonObject>& Obj, const FString& Key, FString& OutValue)
	{
		return Obj.IsValid() && Obj->TryGetStringField(Key, OutValue);
	}

	bool TryGetNumberField(const TSharedPtr<FJsonObject>& Obj, const FString& Key, double& OutValue)
	{
		return UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { Key }, OutValue);
	}

	bool TryGetBoolField(const TSharedPtr<FJsonObject>& Obj, const FString& Key, bool& OutValue)
	{
		return Obj.IsValid() && Obj->TryGetBoolField(Key, OutValue);
	}

	void CopyJsonFieldIfPresent(const TSharedPtr<FJsonObject>& Source, const FString& Key, const TSharedPtr<FJsonObject>& Dest)
	{
		if (!Source.IsValid() || !Dest.IsValid())
		{
			return;
		}
		const TSharedPtr<FJsonValue>* Value = Source->Values.Find(Key);
		if (Value && Value->IsValid())
		{
			Dest->Values.Add(Key, *Value);
		}
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

	static bool TryReadRotatorObject(const TSharedPtr<FJsonObject>& Obj, FRotator& OutValue)
	{
		if (!Obj.IsValid())
		{
			return false;
		}
		double Pitch = 0.0;
		double Yaw = 0.0;
		double Roll = 0.0;
		if (!UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { TEXT("pitch"), TEXT("Pitch") }, Pitch)
			|| !UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { TEXT("yaw"), TEXT("Yaw") }, Yaw)
			|| !UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { TEXT("roll"), TEXT("Roll") }, Roll))
		{
			return false;
		}
		OutValue = FRotator(Pitch, Yaw, Roll);
		return true;
	}

	bool TryReadVectorField(const TSharedPtr<FJsonObject>& Obj, const FString& Key, FVector& OutValue)
	{
		TSharedPtr<FJsonObject> VectorObj;
		if (TryGetObjectField(Obj, Key, VectorObj))
		{
			return TryReadVectorObject(VectorObj, OutValue);
		}
		const TSharedPtr<FJsonValue>* FieldValue = UeAgentJsonDiagnostics::FindFieldValue(Obj, Key);
		return FieldValue && UeAgentJsonDiagnostics::TryReadVectorValue(*FieldValue, OutValue);
	}

	bool TryReadRotatorField(const TSharedPtr<FJsonObject>& Obj, const FString& Key, FRotator& OutValue)
	{
		TSharedPtr<FJsonObject> RotatorObj;
		if (TryGetObjectField(Obj, Key, RotatorObj))
		{
			return TryReadRotatorObject(RotatorObj, OutValue);
		}
		const TSharedPtr<FJsonValue>* FieldValue = UeAgentJsonDiagnostics::FindFieldValue(Obj, Key);
		return FieldValue && UeAgentJsonDiagnostics::TryReadRotatorValue(*FieldValue, OutValue);
	}

	bool HasUserFields(const TSharedPtr<FJsonObject>& Obj)
	{
		return Obj.IsValid() && Obj->Values.Num() > 0;
	}

	bool FindWidgetByTypeRecursive(const TSharedRef<SWidget>& RootWidget, const FName& TypeName, TSharedPtr<SWidget>& OutWidget)
	{
		if (RootWidget->GetType() == TypeName)
		{
			OutWidget = RootWidget;
			return true;
		}

		FChildren* Children = RootWidget->GetChildren();
		if (!Children)
		{
			return false;
		}

		for (int32 ChildIndex = 0; ChildIndex < Children->Num(); ++ChildIndex)
		{
			if (FindWidgetByTypeRecursive(Children->GetChildAt(ChildIndex), TypeName, OutWidget))
			{
				return true;
			}
		}
		return false;
	}

	FString NormalizeToken(FString Value)
	{
		Value.TrimStartAndEndInline();
		Value.ToLowerInline();
		Value.ReplaceInline(TEXT("-"), TEXT("_"));
		return Value;
	}

	FString ViewModeToString(const EViewModeIndex ViewMode)
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

	bool TryParseViewMode(const FString& InValue, EViewModeIndex& OutViewMode)
	{
		const FString Value = NormalizeToken(InValue);
		if (Value == TEXT("lit")) { OutViewMode = VMI_Lit; return true; }
		if (Value == TEXT("unlit")) { OutViewMode = VMI_Unlit; return true; }
		if (Value == TEXT("wireframe") || Value == TEXT("brush_wireframe")) { OutViewMode = VMI_BrushWireframe; return true; }
		if (Value == TEXT("csg_wireframe")) { OutViewMode = VMI_Wireframe; return true; }
		if (Value == TEXT("lit_wireframe")) { OutViewMode = VMI_Lit_Wireframe; return true; }
		if (Value == TEXT("detail_lighting")) { OutViewMode = VMI_Lit_DetailLighting; return true; }
		if (Value == TEXT("lighting_only")) { OutViewMode = VMI_LightingOnly; return true; }
		if (Value == TEXT("reflections") || Value == TEXT("reflection_override")) { OutViewMode = VMI_ReflectionOverride; return true; }
		if (Value == TEXT("player_collision") || Value == TEXT("pawn_collision")) { OutViewMode = VMI_CollisionPawn; return true; }
		if (Value == TEXT("visibility_collision")) { OutViewMode = VMI_CollisionVisibility; return true; }
		return false;
	}

	FViewportTarget ParseTarget(const TSharedPtr<FJsonObject>& Params)
	{
		FViewportTarget Target;
		TSharedPtr<FJsonObject> TargetObj;
		const TSharedPtr<FJsonObject>& Source = TryGetObjectField(Params, TEXT("target"), TargetObj) ? TargetObj : Params;
		TryGetStringField(Source, TEXT("scope"), Target.Scope);
		TryGetStringField(Source, TEXT("asset_path"), Target.AssetPath);
		TryGetStringField(Source, TEXT("asset_type"), Target.AssetType);
		TryGetStringField(Source, TEXT("viewport_role"), Target.ViewportRole);
		TryGetStringField(Source, TEXT("viewport"), Target.Viewport);
		TryGetStringField(Source, TEXT("viewport_id"), Target.ViewportId);
		TryGetBoolField(Source, TEXT("open_editor_if_needed"), Target.bOpenEditorIfNeeded);
		if (Target.Scope.IsEmpty())
		{
			Target.Scope = (!Target.AssetPath.IsEmpty() || !Target.AssetType.IsEmpty()) ? TEXT("asset_editor") : TEXT("level");
		}
		Target.Scope = NormalizeToken(Target.Scope);
		Target.AssetType = NormalizeToken(Target.AssetType);
		Target.ViewportRole = NormalizeToken(Target.ViewportRole.IsEmpty() ? TEXT("primary") : Target.ViewportRole);
		Target.Viewport = NormalizeToken(Target.Viewport);
		if (Target.Viewport.IsEmpty() && Target.Scope != TEXT("level"))
		{
			Target.Viewport = TEXT("active");
		}
		return Target;
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

	TSharedPtr<FJsonObject> MakeLevelVisualOptionsJson(FLevelEditorViewportClient* Client)
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

	bool TryReadBoolOption(const TSharedPtr<FJsonObject>& Options, const FString& Key, bool& OutValue)
	{
		if (TryGetBoolField(Options, Key, OutValue))
		{
			return true;
		}
		TSharedPtr<FJsonObject> Overlays;
		return TryGetObjectField(Options, TEXT("overlays"), Overlays) && TryGetBoolField(Overlays, Key, OutValue);
	}

	TSharedPtr<FJsonObject> MakePixelSignalJson(const TArray<FColor>& Pixels, const FIntPoint& Size)
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

	FResolvedScreenshotOptions::FResolvedScreenshotOptions()
		: Format(TEXT("png"))
		, Quality(90)
		, MaxSize(1600)
		, WarmupFrames(2)
		, bAllowWindowMutation(true)
		, bRestoreWindowAfterCapture(true)
		, bTickEditorWorld(false)
		, EditorWorldTickFrames(0)
		, EditorWorldTickDeltaSeconds(1.0f / 60.0f)
	{
	}


	bool ParseScreenshotOptions(const TSharedPtr<FJsonObject>& Params, FResolvedScreenshotOptions& OutOptions, FString& OutError)
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
		bool bAllowWindowMutation = true;
		TryGetBoolField(Params, TEXT("allow_window_mutation"), bAllowWindowMutation);
		bool bRestoreWindowAfterCapture = true;
		TryGetBoolField(Params, TEXT("restore_window_after_capture"), bRestoreWindowAfterCapture);
		bool bTickEditorWorld = false;
		TryGetBoolField(Params, TEXT("tick_editor_world"), bTickEditorWorld);
		int32 EditorWorldTickFrames = 0;
		double TickFramesD = 0.0;
		if (TryGetNumberField(Params, TEXT("editor_world_tick_frames"), TickFramesD))
		{
			EditorWorldTickFrames = FMath::Clamp(static_cast<int32>(TickFramesD), 0, 240);
		}
		EditorWorldTickFrames = bTickEditorWorld ? EditorWorldTickFrames : 0;
		double TickDeltaD = 1.0 / 60.0;
		TryGetNumberField(Params, TEXT("editor_world_tick_delta_seconds"), TickDeltaD);
		const float EditorWorldTickDeltaSeconds = FMath::Clamp(static_cast<float>(TickDeltaD), 1.0f / 240.0f, 1.0f / 5.0f);

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
		OutOptions.bAllowWindowMutation = bAllowWindowMutation;
		OutOptions.bRestoreWindowAfterCapture = bRestoreWindowAfterCapture;
		OutOptions.bTickEditorWorld = bTickEditorWorld;
		OutOptions.EditorWorldTickFrames = EditorWorldTickFrames;
		OutOptions.EditorWorldTickDeltaSeconds = EditorWorldTickDeltaSeconds;
		return true;
	}

	bool CaptureEditorViewportClientBackbuffer(
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
		FSceneViewport* SceneViewport = static_cast<FSceneViewport*>(Viewport);
		TSharedPtr<SWindow> CaptureWindow;
		bool bCaptureWindowResolved = false;
		bool bWindowVisibleBefore = false;
		bool bWindowMinimizedBefore = false;
		bool bWindowMutatedForCapture = false;
		if (FSlateApplication::IsInitialized())
		{
			if (const TSharedPtr<SViewport> ViewportWidget = SceneViewport->GetViewportWidget().Pin())
			{
				CaptureWindow = FSlateApplication::Get().FindWidgetWindow(ViewportWidget.ToSharedRef());
			}
			if (CaptureWindow.IsValid())
			{
				bCaptureWindowResolved = true;
				bWindowVisibleBefore = CaptureWindow->IsVisible();
				bWindowMinimizedBefore = CaptureWindow->IsWindowMinimized();
				if (bWindowMinimizedBefore)
				{
					if (!Options.bAllowWindowMutation)
					{
						OutError = TEXT("viewport_window_minimized_ui_mutation_disabled");
						return false;
					}
					CaptureWindow->Restore();
					bWindowMutatedForCapture = true;
					FSlateApplication::Get().PumpMessages();
				}
				if (!CaptureWindow->IsVisible())
				{
					if (!Options.bAllowWindowMutation)
					{
						OutError = TEXT("viewport_window_hidden_ui_mutation_disabled");
						return false;
					}
					CaptureWindow->ShowWindow();
					bWindowMutatedForCapture = true;
				}
				if (Options.bAllowWindowMutation)
				{
					CaptureWindow->BringToFront(true);
					bWindowMutatedForCapture = true;
					FSlateApplication::Get().PumpMessages();
				}
			}
		}
		auto RestoreCaptureWindowIfNeeded = [&CaptureWindow, bWindowMinimizedBefore, &Options]()
		{
			if (Options.bRestoreWindowAfterCapture && bWindowMinimizedBefore && CaptureWindow.IsValid())
			{
				CaptureWindow->Minimize();
				if (FSlateApplication::IsInitialized())
				{
					FSlateApplication::Get().PumpMessages();
				}
			}
		};
		int32 DrawsPerformed = 0;
		int32 EditorWorldTicksPerformed = 0;
		if (Options.EditorWorldTickFrames > 0)
		{
			UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (!EditorWorld)
			{
				OutError = TEXT("no_editor_world_for_warmup_tick");
				RestoreCaptureWindowIfNeeded();
				return false;
			}
			for (int32 TickIndex = 0; TickIndex < Options.EditorWorldTickFrames; ++TickIndex)
			{
				EditorWorld->Tick(LEVELTICK_All, Options.EditorWorldTickDeltaSeconds);
				++EditorWorldTicksPerformed;
			}
		}
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
			RestoreCaptureWindowIfNeeded();
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
			RestoreCaptureWindowIfNeeded();
			return false;
		}

		OutPixels = MoveTemp(Pixels);
		OutSize = Size;
		if (bAppliedFallbackViewportSize)
		{
			SceneViewport->SetFixedViewportSize(0, 0);
		}
		RestoreCaptureWindowIfNeeded();

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
		OutCaptureInfo->SetBoolField(TEXT("editor_world_tick_requested"), Options.bTickEditorWorld);
		OutCaptureInfo->SetNumberField(TEXT("editor_world_tick_frames_requested"), Options.EditorWorldTickFrames);
		OutCaptureInfo->SetNumberField(TEXT("editor_world_ticks_performed"), EditorWorldTicksPerformed);
		OutCaptureInfo->SetNumberField(TEXT("editor_world_tick_delta_seconds"), Options.EditorWorldTickDeltaSeconds);
		OutCaptureInfo->SetBoolField(TEXT("allow_window_mutation"), Options.bAllowWindowMutation);
		OutCaptureInfo->SetBoolField(TEXT("restore_window_after_capture"), Options.bRestoreWindowAfterCapture);
		OutCaptureInfo->SetBoolField(TEXT("capture_window_resolved"), bCaptureWindowResolved);
		OutCaptureInfo->SetBoolField(TEXT("capture_window_visible_before"), bWindowVisibleBefore);
		OutCaptureInfo->SetBoolField(TEXT("capture_window_minimized_before"), bWindowMinimizedBefore);
		OutCaptureInfo->SetBoolField(TEXT("capture_window_mutated"), bWindowMutatedForCapture);
		if (CaptureWindow.IsValid())
		{
			OutCaptureInfo->SetBoolField(TEXT("capture_window_visible_after"), CaptureWindow->IsVisible());
			OutCaptureInfo->SetBoolField(TEXT("capture_window_minimized_after"), CaptureWindow->IsWindowMinimized());
		}
		OutCaptureInfo->SetStringField(TEXT("view_mode_name"), ViewModeToString(Client->GetViewMode()));
		OutCaptureInfo->SetNumberField(TEXT("view_mode_value"), static_cast<int32>(Client->GetViewMode()));
		return true;
	}

	bool ParseCameraRequest(const TSharedPtr<FJsonObject>& Params, const FViewportCamera& CurrentCamera, FViewportCamera& OutCamera, FString& OutError)
	{
		TSharedPtr<FJsonObject> CameraObj;
		const TSharedPtr<FJsonObject>& Source = TryGetObjectField(Params, TEXT("camera"), CameraObj) ? CameraObj : Params;
		if (!Source.IsValid())
		{
			OutError = TEXT("missing_camera");
			return false;
		}

		OutCamera = CurrentCamera;
		OutCamera.bValid = true;

		TSharedPtr<FJsonObject> OrbitObj;
		if (TryGetObjectField(Source, TEXT("orbit"), OrbitObj))
		{
			FVector Target = FVector::ZeroVector;
			TryReadVectorField(OrbitObj, TEXT("target"), Target);

			double Yaw = CurrentCamera.Rotation.Yaw;
			double Pitch = CurrentCamera.Rotation.Pitch;
			double Roll = 0.0;
			double Distance = 500.0;
			double Fov = CurrentCamera.Fov;
			TryGetNumberField(OrbitObj, TEXT("yaw"), Yaw);
			TryGetNumberField(OrbitObj, TEXT("pitch"), Pitch);
			TryGetNumberField(OrbitObj, TEXT("roll"), Roll);
			TryGetNumberField(OrbitObj, TEXT("distance"), Distance);
			TryGetNumberField(OrbitObj, TEXT("fov"), Fov);

			const FRotator Rotation(Pitch, Yaw, Roll);
			OutCamera.Rotation = Rotation;
			OutCamera.Location = Target - Rotation.Vector() * static_cast<float>(FMath::Max(Distance, 1.0));
			OutCamera.Fov = static_cast<float>(FMath::Clamp(Fov, 5.0, 170.0));
			return true;
		}

		bool bAnyField = false;
		bAnyField |= TryReadVectorField(Source, TEXT("location"), OutCamera.Location);
		bAnyField |= TryReadRotatorField(Source, TEXT("rotation"), OutCamera.Rotation);
		double Fov = OutCamera.Fov;
		if (TryGetNumberField(Source, TEXT("fov"), Fov))
		{
			OutCamera.Fov = static_cast<float>(FMath::Clamp(Fov, 5.0, 170.0));
			bAnyField = true;
		}
		FString Projection;
		if (TryGetStringField(Source, TEXT("projection"), Projection))
		{
			OutCamera.Projection = NormalizeToken(Projection);
			bAnyField = true;
		}
		if (!bAnyField)
		{
			OutError = TEXT("missing_camera_fields");
			return false;
		}
		return true;
	}
}
