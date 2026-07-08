// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commands/EditorViewport/UeAgentEditorViewportStaticMesh.h"

#include "Utilities/UeAgentJsonDiagnostics.h"
#include "UeAgentInterfaceSettings.h"
#include "Server/UeAgentHttpServer.h"

#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSocket.h"
#include "Framework/Commands/InputBindingManager.h"
#include "Framework/Commands/UICommandList.h"
#include "HAL/FileManager.h"
#include "IStaticMeshEditor.h"
#include "LevelEditorViewport.h"
#include "Misc/App.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
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

	static bool TryReadRotatorField(const TSharedPtr<FJsonObject>& Obj, const FString& Key, FRotator& OutValue)
	{
		TSharedPtr<FJsonObject> RotatorObj;
		if (TryGetObjectField(Obj, Key, RotatorObj))
		{
			return TryReadRotatorObject(RotatorObj, OutValue);
		}
		const TSharedPtr<FJsonValue>* FieldValue = UeAgentJsonDiagnostics::FindFieldValue(Obj, Key);
		return FieldValue && UeAgentJsonDiagnostics::TryReadRotatorValue(*FieldValue, OutValue);
	}

	static bool HasUserFields(const TSharedPtr<FJsonObject>& Obj)
	{
		return Obj.IsValid() && Obj->Values.Num() > 0;
	}

	static bool FindWidgetByTypeRecursive(const TSharedRef<SWidget>& RootWidget, const FName& TypeName, TSharedPtr<SWidget>& OutWidget)
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

	static FViewportTarget ParseTarget(const TSharedPtr<FJsonObject>& Params)
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
		Target.Viewport = NormalizeToken(Target.Viewport.IsEmpty() ? TEXT("active") : Target.Viewport);
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

	static bool TryReadBoolOption(const TSharedPtr<FJsonObject>& Options, const FString& Key, bool& OutValue)
	{
		if (TryGetBoolField(Options, Key, OutValue))
		{
			return true;
		}
		TSharedPtr<FJsonObject> Overlays;
		return TryGetObjectField(Options, TEXT("overlays"), Overlays) && TryGetBoolField(Overlays, Key, OutValue);
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

	static bool ParseCameraRequest(const TSharedPtr<FJsonObject>& Params, const FViewportCamera& CurrentCamera, FViewportCamera& OutCamera, FString& OutError)
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

	class FStaticMeshViewportAdapter final : public IViewportAdapter
	{
	public:
		virtual FString GetName() const override
		{
			return TEXT("static_mesh");
		}

		virtual bool CanResolve(const FResolveRequest& Request) const override
		{
			if (Request.Target.Scope != TEXT("asset_editor"))
			{
				return false;
			}
			return Request.Target.AssetType.IsEmpty()
				|| Request.Target.AssetType == TEXT("static_mesh")
				|| Request.Target.AssetType == TEXT("staticmesh")
				|| Request.Target.AssetType == TEXT("ustaticmesh");
		}

		virtual bool Resolve(const FResolveRequest& Request, FResolvedViewport& OutViewport, FString& OutError) const override
		{
			if (Request.Target.AssetPath.IsEmpty())
			{
				OutError = TEXT("missing_asset_path");
				return false;
			}

			UStaticMesh* Mesh = LoadStaticMeshAsset(Request.Target.AssetPath);
			if (!Mesh)
			{
				OutError = TEXT("static_mesh_not_found");
				return false;
			}

			IStaticMeshEditor* MeshEditor = GetStaticMeshEditor(Mesh, Request.Target.bOpenEditorIfNeeded);
			if (!MeshEditor)
			{
				OutError = TEXT("static_mesh_editor_not_opened");
				return false;
			}

			FEditorViewportClient& Client = MeshEditor->GetViewportClient();
			OutViewport.Target = Request.Target;
			OutViewport.Target.Scope = TEXT("asset_editor");
			OutViewport.Target.AssetPath = Mesh->GetPathName();
			OutViewport.Target.AssetType = TEXT("static_mesh");
			OutViewport.Target.ViewportRole = TEXT("primary");
			OutViewport.Target.ViewportId = FString::Printf(TEXT("asset:%s#static_mesh_primary"), *Mesh->GetPathName());
			OutViewport.AdapterName = GetName();
			OutViewport.ClientClass = TEXT("FStaticMeshEditorViewportClient");
			OutViewport.ViewportClient = &Client;
			OutViewport.Asset = Mesh;
			return true;
		}

		virtual bool ReadState(const FResolvedViewport& Viewport, FViewportState& OutState, FString& OutError) const override
		{
			IStaticMeshEditor* MeshEditor = GetEditorFromResolved(Viewport, OutError);
			if (!MeshEditor)
			{
				return false;
			}
			FEditorViewportClient* Client = Viewport.ViewportClient;
			if (!Client)
			{
				OutError = TEXT("invalid_static_mesh_viewport");
				return false;
			}

			OutState.Target = Viewport.Target;
			OutState.ClientClass = TEXT("FStaticMeshEditorViewportClient");
			OutState.Adapter = GetName();
			OutState.Camera.Location = Client->GetViewLocation();
			OutState.Camera.Rotation = Client->GetViewRotation();
			OutState.Camera.Fov = Client->ViewFOV;
			OutState.Camera.Space = TEXT("asset_preview_world");
			OutState.Camera.Projection = Client->ViewportType == LVT_Perspective ? TEXT("perspective") : TEXT("orthographic");
			OutState.Camera.bValid = true;
			OutState.Capabilities.bCamera = true;
			OutState.Capabilities.bVisualOptions = true;
			OutState.Capabilities.bPreviewState = false;
			OutState.Capabilities.bScreenshot = true;
			OutState.Capabilities.FocusModes = { TEXT("asset_bounds"), TEXT("bounds"), TEXT("selection"), TEXT("pivot"), TEXT("socket"), TEXT("custom_bounds"), TEXT("custom_point") }; OutState.Capabilities.ScreenshotBackends = { TEXT("static_mesh_editor_backbuffer") }; OutState.Capabilities.VisualOptionKeys = { TEXT("view_mode"), TEXT("grid"), TEXT("bounds"), TEXT("pivot"), TEXT("normals"), TEXT("tangents"), TEXT("binormals"), TEXT("vertices"), TEXT("simple_collision"), TEXT("complex_collision"), TEXT("collision") };
			OutState.VisualOptions = MakeStaticMeshVisualOptionsJson(*MeshEditor, *Client);
			OutState.PreviewState = MakeStaticMeshPreviewStateJson(*MeshEditor);
			return true;
		}

		virtual bool WriteCamera(const FResolvedViewport& Viewport, const FViewportCamera& Camera, FString& OutError) const override
		{
			IStaticMeshEditor* MeshEditor = GetEditorFromResolved(Viewport, OutError);
			if (!MeshEditor)
			{
				return false;
			}
			FEditorViewportClient* Client = Viewport.ViewportClient;
			if (!Client)
			{
				OutError = TEXT("invalid_static_mesh_viewport");
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
			MeshEditor->RefreshViewport();
			return true;
		}

		virtual bool Focus(const FResolvedViewport& Viewport, const TSharedPtr<FJsonObject>& FocusObj, TSharedPtr<FJsonObject>& OutData, FString& OutError) const override
		{
			IStaticMeshEditor* MeshEditor = GetEditorFromResolved(Viewport, OutError);
			if (!MeshEditor)
			{
				return false;
			}
			FEditorViewportClient* Client = Viewport.ViewportClient;
			UStaticMesh* Mesh = Cast<UStaticMesh>(Viewport.Asset);
			if (!Client || !Mesh)
			{
				OutError = TEXT("invalid_static_mesh_viewport");
				return false;
			}

			FString Mode = TEXT("asset_bounds");
			TryGetStringField(FocusObj, TEXT("mode"), Mode);
			Mode = NormalizeToken(Mode);

			FBox FocusBox(ForceInit);
			FVector FocusPoint = FVector::ZeroVector;
			bool bUsePoint = false;
			if (Mode == TEXT("asset_bounds") || Mode == TEXT("bounds"))
			{
				FocusBox = MakeMeshBoundsBox(Mesh);
			}
			else if (Mode == TEXT("selection"))
			{
				if (!BuildSelectionBox(*MeshEditor, FocusBox))
				{
					FocusBox = MakeMeshBoundsBox(Mesh);
					Mode = TEXT("asset_bounds");
				}
			}
			else if (Mode == TEXT("pivot"))
			{
				FocusPoint = FVector::ZeroVector;
				bUsePoint = true;
			}
			else if (Mode == TEXT("socket"))
			{
				FString SocketName;
				if (!TryGetStringField(FocusObj, TEXT("socket"), SocketName) && !TryGetStringField(FocusObj, TEXT("socket_name"), SocketName))
				{
					OutError = TEXT("missing_socket_name");
					return false;
				}
				const UStaticMeshSocket* Socket = FindSocketByName(Mesh, SocketName);
				if (!Socket)
				{
					OutError = TEXT("static_mesh_socket_not_found");
					return false;
				}
				FocusPoint = Socket->RelativeLocation;
				bUsePoint = true;
			}
			else if (Mode == TEXT("custom_bounds"))
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
				FocusBox = FBox(Min, Max);
			}
			else if (Mode == TEXT("custom_point"))
			{
				if (!TryReadVectorField(FocusObj, TEXT("point"), FocusPoint))
				{
					OutError = TEXT("missing_custom_point");
					return false;
				}
				bUsePoint = true;
			}
			else
			{
				OutError = TEXT("unsupported_focus_mode");
				return false;
			}

			double Radius = 50.0;
			TryGetNumberField(FocusObj, TEXT("radius"), Radius);
			bool bInstant = true;
			TryGetBoolField(FocusObj, TEXT("instant"), bInstant);
			if (bUsePoint)
			{
				FocusBox = FBox::BuildAABB(FocusPoint, FVector(FMath::Max(static_cast<float>(Radius), 1.0f)));
			}
			if (!FocusBox.IsValid)
			{
				OutError = TEXT("invalid_focus_bounds");
				return false;
			}

			Client->FocusViewportOnBox(FocusBox, bInstant);
			Client->SetLookAtLocation(FocusBox.GetCenter(), false);
			Client->Invalidate();
			if (Client->Viewport)
			{
				Client->Viewport->Invalidate();
			}
			MeshEditor->RefreshViewport();

			OutData = MakeShared<FJsonObject>();
			OutData->SetStringField(TEXT("mode"), Mode);
			OutData->SetStringField(TEXT("bounds_space"), TEXT("asset_local"));
			OutData->SetBoolField(TEXT("instant"), bInstant);
			TSharedPtr<FJsonObject> BoundsJson = MakeShared<FJsonObject>();
			BoundsJson->SetObjectField(TEXT("min"), MakeVectorJson(FocusBox.Min));
			BoundsJson->SetObjectField(TEXT("max"), MakeVectorJson(FocusBox.Max));
			BoundsJson->SetObjectField(TEXT("center"), MakeVectorJson(FocusBox.GetCenter()));
			BoundsJson->SetObjectField(TEXT("extent"), MakeVectorJson(FocusBox.GetExtent()));
			OutData->SetObjectField(TEXT("bounds"), BoundsJson);
			return true;
		}

		virtual bool ApplyVisualOptions(const FResolvedViewport& Viewport, const TSharedPtr<FJsonObject>& Options, TSharedPtr<FJsonObject>& OutData, FString& OutError) const override
		{
			IStaticMeshEditor* MeshEditor = GetEditorFromResolved(Viewport, OutError);
			if (!MeshEditor)
			{
				return false;
			}
			FEditorViewportClient* Client = Viewport.ViewportClient;
			if (!Client)
			{
				OutError = TEXT("invalid_static_mesh_viewport");
				return false;
			}

			TArray<FString> Unsupported;
			FString ViewModeText;
			if (TryGetStringField(Options, TEXT("view_mode"), ViewModeText))
			{
				EViewModeIndex ViewMode;
				if (!TryParseViewMode(ViewModeText, ViewMode))
				{
					Unsupported.Add(TEXT("view_mode"));
				}
				else
				{
					Client->SetViewMode(ViewMode);
				}
			}

			ApplyStaticMeshCommandBoolOption(*MeshEditor, Options, TEXT("grid"), FName(TEXT("ToggleShowGrids")), Unsupported);
			ApplyStaticMeshCommandBoolOption(*MeshEditor, Options, TEXT("bounds"), FName(TEXT("ToggleShowBounds")), Unsupported);
			ApplyStaticMeshCommandBoolOption(*MeshEditor, Options, TEXT("pivot"), FName(TEXT("ToggleShowPivots")), Unsupported);
			ApplyStaticMeshCommandBoolOption(*MeshEditor, Options, TEXT("normals"), FName(TEXT("ToggleShowNormals")), Unsupported);
			ApplyStaticMeshCommandBoolOption(*MeshEditor, Options, TEXT("tangents"), FName(TEXT("ToggleShowTangents")), Unsupported);
			ApplyStaticMeshCommandBoolOption(*MeshEditor, Options, TEXT("binormals"), FName(TEXT("ToggleShowBinormals")), Unsupported);
			ApplyStaticMeshCommandBoolOption(*MeshEditor, Options, TEXT("vertices"), FName(TEXT("ToggleShowVertices")), Unsupported);

			bool bCollision = false;
			if (TryReadBoolOption(Options, TEXT("collision"), bCollision))
			{
				const bool bSimpleApplied = SetStaticMeshCommandBool(*MeshEditor, FName(TEXT("ToggleShowSimpleCollisions")), bCollision);
				const bool bComplexApplied = SetStaticMeshCommandBool(*MeshEditor, FName(TEXT("ToggleShowComplexCollisions")), bCollision);
				if (!bSimpleApplied || !bComplexApplied)
				{
					Unsupported.AddUnique(TEXT("collision"));
				}
			}
			ApplyStaticMeshCommandBoolOption(*MeshEditor, Options, TEXT("simple_collision"), FName(TEXT("ToggleShowSimpleCollisions")), Unsupported);
			ApplyStaticMeshCommandBoolOption(*MeshEditor, Options, TEXT("complex_collision"), FName(TEXT("ToggleShowComplexCollisions")), Unsupported);

			bool bRealtime = Client->IsRealtime();
			if (TryGetBoolField(Options, TEXT("realtime"), bRealtime))
			{
				Client->SetRealtime(bRealtime);
			}

			Client->Invalidate();
			if (Client->Viewport)
			{
				Client->Viewport->Invalidate();
			}
			MeshEditor->RefreshViewport();

			OutData = MakeStaticMeshVisualOptionsJson(*MeshEditor, *Client);
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

		virtual bool ApplyPreviewState(const FResolvedViewport&, const TSharedPtr<FJsonObject>& PreviewState, TSharedPtr<FJsonObject>& OutData, FString& OutError) const override
		{
			OutData = MakeShared<FJsonObject>();
			if (!HasUserFields(PreviewState))
			{
				OutData->SetBoolField(TEXT("applied"), false);
				OutData->SetStringField(TEXT("reason"), TEXT("empty_preview_state"));
				return true;
			}
			OutError = TEXT("editor_viewport_preview_state_not_supported");
			return false;
		}

		virtual bool CaptureScreenshot(const FResolvedViewport& Viewport, const TSharedPtr<FJsonObject>& Params, FViewportScreenshotResult& OutResult, FString& OutError) const override
		{
			IStaticMeshEditor* MeshEditor = GetEditorFromResolved(Viewport, OutError);
			if (!MeshEditor)
			{
				return false;
			}
			if (!Viewport.ViewportClient)
			{
				OutError = TEXT("invalid_static_mesh_viewport");
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
			OutResult.CaptureBackend = TEXT("static_mesh_editor_backbuffer");
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
			if (!WriteCamera(Viewport, Snapshot.State.Camera, OutError))
			{
				return false;
			}
			TSharedPtr<FJsonObject> Applied;
			return ApplyVisualOptions(Viewport, Snapshot.State.VisualOptions, Applied, OutError);
		}

	private:
		static UStaticMesh* LoadStaticMeshAsset(FString AssetPath)
		{
			AssetPath.TrimStartAndEndInline();
			if (AssetPath.IsEmpty())
			{
				return nullptr;
			}
			if (UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *AssetPath))
			{
				return Mesh;
			}
			if (!AssetPath.Contains(TEXT(".")))
			{
				const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *AssetPath, *FPackageName::GetShortName(AssetPath));
				return LoadObject<UStaticMesh>(nullptr, *ObjectPath);
			}
			return nullptr;
		}

		static IStaticMeshEditor* GetStaticMeshEditor(UStaticMesh* Mesh, const bool bOpenIfNeeded)
		{
			if (!Mesh || !GEditor)
			{
				return nullptr;
			}
			UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
			if (!AssetEditorSubsystem)
			{
				return nullptr;
			}
			if (bOpenIfNeeded)
			{
				AssetEditorSubsystem->OpenEditorForAsset(Mesh);
			}
			IAssetEditorInstance* EditorInstance = AssetEditorSubsystem->FindEditorForAsset(Mesh, false);
			if (!EditorInstance || EditorInstance->GetEditorName() != FName(TEXT("StaticMeshEditor")))
			{
				return nullptr;
			}
			FAssetEditorToolkit* Toolkit = static_cast<FAssetEditorToolkit*>(EditorInstance);
			return Toolkit ? static_cast<IStaticMeshEditor*>(Toolkit) : nullptr;
		}

		static IStaticMeshEditor* GetEditorFromResolved(const FResolvedViewport& Viewport, FString& OutError)
		{
			UStaticMesh* Mesh = Cast<UStaticMesh>(Viewport.Asset);
			IStaticMeshEditor* MeshEditor = GetStaticMeshEditor(Mesh, false);
			if (!MeshEditor)
			{
				OutError = TEXT("static_mesh_editor_not_opened");
				return nullptr;
			}
			return MeshEditor;
		}

		static FBox MakeMeshBoundsBox(const UStaticMesh* Mesh)
		{
			const FBoxSphereBounds Bounds = Mesh ? Mesh->GetBounds() : FBoxSphereBounds(EForceInit::ForceInit);
			return FBox(Bounds.Origin - Bounds.BoxExtent, Bounds.Origin + Bounds.BoxExtent);
		}

		static bool BuildSelectionBox(IStaticMeshEditor& MeshEditor, FBox& OutBox)
		{
			FBox Box(ForceInit);
			for (UStaticMeshSocket* Socket : MeshEditor.GetSelectedSockets())
			{
				if (Socket)
				{
					Box += Socket->RelativeLocation;
				}
			}
			FBox PrimBox(ForceInit);
			if (MeshEditor.CalcSelectedPrimsAABB(PrimBox) && PrimBox.IsValid)
			{
				Box += PrimBox;
			}
			OutBox = Box;
			return OutBox.IsValid != 0;
		}

		static const UStaticMeshSocket* FindSocketByName(const UStaticMesh* Mesh, const FString& SocketName)
		{
			if (!Mesh)
			{
				return nullptr;
			}
			const FName Name(*SocketName);
			for (const TObjectPtr<UStaticMeshSocket>& Socket : Mesh->Sockets)
			{
				if (Socket && Socket->SocketName == Name)
				{
					return Socket.Get();
				}
			}
			return nullptr;
		}

		static TSharedPtr<FJsonObject> MakeStaticMeshVisualOptionsJson(IStaticMeshEditor& MeshEditor, FEditorViewportClient& Client)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("view_mode"), ViewModeToString(Client.GetViewMode()));
			Obj->SetNumberField(TEXT("view_mode_value"), static_cast<int32>(Client.GetViewMode()));
			TSharedPtr<FJsonObject> Overlays = MakeShared<FJsonObject>();
			const bool bGrid = ReadStaticMeshCommandBoolOrDefault(MeshEditor, FName(TEXT("ToggleShowGrids")), Client.IsSetShowGridChecked());
			const bool bBounds = ReadStaticMeshCommandBoolOrDefault(MeshEditor, FName(TEXT("ToggleShowBounds")), Client.IsSetShowBoundsChecked());
			const bool bPivot = ReadStaticMeshCommandBoolOrDefault(MeshEditor, FName(TEXT("ToggleShowPivots")), false);
			const bool bNormals = ReadStaticMeshCommandBoolOrDefault(MeshEditor, FName(TEXT("ToggleShowNormals")), false);
			const bool bTangents = ReadStaticMeshCommandBoolOrDefault(MeshEditor, FName(TEXT("ToggleShowTangents")), false);
			const bool bBinormals = ReadStaticMeshCommandBoolOrDefault(MeshEditor, FName(TEXT("ToggleShowBinormals")), false);
			const bool bVertices = ReadStaticMeshCommandBoolOrDefault(MeshEditor, FName(TEXT("ToggleShowVertices")), false);
			const bool bSimpleCollision = ReadStaticMeshCommandBoolOrDefault(MeshEditor, FName(TEXT("ToggleShowSimpleCollisions")), Client.IsSetShowCollisionChecked());
			const bool bComplexCollision = ReadStaticMeshCommandBoolOrDefault(MeshEditor, FName(TEXT("ToggleShowComplexCollisions")), Client.IsSetShowCollisionChecked());
			Overlays->SetBoolField(TEXT("grid"), bGrid);
			Overlays->SetBoolField(TEXT("bounds"), bBounds);
			Overlays->SetBoolField(TEXT("pivot"), bPivot);
			Overlays->SetBoolField(TEXT("normals"), bNormals);
			Overlays->SetBoolField(TEXT("tangents"), bTangents);
			Overlays->SetBoolField(TEXT("binormals"), bBinormals);
			Overlays->SetBoolField(TEXT("vertices"), bVertices);
			Overlays->SetBoolField(TEXT("simple_collision"), bSimpleCollision);
			Overlays->SetBoolField(TEXT("complex_collision"), bComplexCollision);
			Overlays->SetBoolField(TEXT("collision"), bSimpleCollision || bComplexCollision);
			Obj->SetObjectField(TEXT("overlays"), Overlays);
			Obj->SetBoolField(TEXT("realtime"), Client.IsRealtime());
			return Obj;
		}

		static TSharedPtr<FJsonObject> MakeStaticMeshPreviewStateJson(IStaticMeshEditor& MeshEditor)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			if (UStaticMesh* Mesh = MeshEditor.GetStaticMesh())
			{
				Obj->SetStringField(TEXT("asset_path"), Mesh->GetPathName());
			}
			Obj->SetNumberField(TEXT("lod_index"), MeshEditor.GetCurrentLODIndex());
			Obj->SetNumberField(TEXT("lod_level"), MeshEditor.GetCurrentLODLevel());
			Obj->SetNumberField(TEXT("selected_socket_count"), MeshEditor.GetSelectedSockets().Num());
			Obj->SetBoolField(TEXT("has_selected_collision_primitives"), MeshEditor.HasSelectedPrims());
			return Obj;
		}

		static TSharedPtr<FUICommandInfo> FindMappedStaticMeshCommand(const TSharedRef<FUICommandList>& CommandList, const FName PrimaryCommandName, const FName FallbackCommandName = NAME_None)
		{
			const FName StaticMeshCommandContext(TEXT("StaticMeshEditor"));
			auto TryFindMappedCommand = [&CommandList, &StaticMeshCommandContext](const FName CommandName) -> TSharedPtr<FUICommandInfo>
			{
				if (CommandName.IsNone())
				{
					return nullptr;
				}
				const TSharedPtr<FUICommandInfo> Command = FInputBindingManager::Get().FindCommandInContext(StaticMeshCommandContext, CommandName);
				if (Command.IsValid() && CommandList->IsActionMapped(Command))
				{
					return Command;
				}
				return nullptr;
			};

			if (TSharedPtr<FUICommandInfo> PrimaryCommand = TryFindMappedCommand(PrimaryCommandName))
			{
				return PrimaryCommand;
			}
			return TryFindMappedCommand(FallbackCommandName);
		}

		static bool TryReadStaticMeshCommandBool(IStaticMeshEditor& MeshEditor, const FName PrimaryCommandName, bool& OutValue, const FName FallbackCommandName = NAME_None)
		{
			const TSharedRef<FUICommandList> CommandList = MeshEditor.GetToolkitCommands();
			const TSharedPtr<FUICommandInfo> Command = FindMappedStaticMeshCommand(CommandList, PrimaryCommandName, FallbackCommandName);
			if (!Command.IsValid())
			{
				return false;
			}
			OutValue = CommandList->GetCheckState(Command.ToSharedRef()) == ECheckBoxState::Checked;
			return true;
		}

		static bool ReadStaticMeshCommandBoolOrDefault(IStaticMeshEditor& MeshEditor, const FName PrimaryCommandName, const bool bDefaultValue, const FName FallbackCommandName = NAME_None)
		{
			bool bValue = bDefaultValue;
			TryReadStaticMeshCommandBool(MeshEditor, PrimaryCommandName, bValue, FallbackCommandName);
			return bValue;
		}

		static bool SetStaticMeshCommandBool(IStaticMeshEditor& MeshEditor, const FName PrimaryCommandName, const bool bValue, const FName FallbackCommandName = NAME_None)
		{
			const TSharedRef<FUICommandList> CommandList = MeshEditor.GetToolkitCommands();
			const TSharedPtr<FUICommandInfo> Command = FindMappedStaticMeshCommand(CommandList, PrimaryCommandName, FallbackCommandName);
			if (!Command.IsValid())
			{
				return false;
			}

			const bool bCurrentValue = CommandList->GetCheckState(Command.ToSharedRef()) == ECheckBoxState::Checked;
			if (bCurrentValue != bValue)
			{
				return CommandList->TryExecuteAction(Command.ToSharedRef());
			}
			return true;
		}

		static void ApplyStaticMeshCommandBoolOption(
			IStaticMeshEditor& MeshEditor,
			const TSharedPtr<FJsonObject>& Options,
			const FString& Key,
			const FName PrimaryCommandName,
			TArray<FString>& Unsupported,
			const FName FallbackCommandName = NAME_None)
		{
			bool bValue = false;
			if (TryReadBoolOption(Options, Key, bValue))
			{
				if (!SetStaticMeshCommandBool(MeshEditor, PrimaryCommandName, bValue, FallbackCommandName))
				{
					Unsupported.AddUnique(Key);
				}
			}
		}
	};


	TSharedRef<IViewportAdapter> MakeStaticMeshViewportAdapter()
	{
		return MakeShared<FStaticMeshViewportAdapter>();
	}
}
