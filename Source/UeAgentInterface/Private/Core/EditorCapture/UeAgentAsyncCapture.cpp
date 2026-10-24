#include "Core/EditorCapture/UeAgentAsyncCapture.h"

#include "Camera/CameraTypes.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Dom/JsonValue.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"
#include "RHIGPUReadback.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "RenderDeferredCleanup.h"
#include "Slate/WidgetRenderer.h"
#include "TextureResource.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/SWidget.h"
#include "Widgets/SWindow.h"

namespace UE::UAI::EditorCapture
{
	void ConfigureCameraCapture(USceneCaptureComponent2D& Capture, const FMinimalViewInfo& View, const bool bCameraCut)
	{
		check(IsInGameThread());
		Capture.CaptureSource = SCS_FinalColorLDR;
		Capture.bCaptureEveryFrame = false;
		Capture.bCaptureOnMovement = false;
		// A one-shot capture still needs a view state for native eye adaptation.
		Capture.bAlwaysPersistRenderingState = true;
		Capture.SetCameraView(View);
		Capture.PostProcessSettings = View.PostProcessSettings;
		Capture.PostProcessBlendWeight = View.PostProcessBlendWeight;
		Capture.bCameraCutThisFrame = bCameraCut;
	}

	struct FAsyncCaptureFrame::FState
	{
		FCriticalSection Mutex;
		TUniquePtr<FRHIGPUTextureReadback> Readback; // render thread only
		TArray<FColor> Pixels;
		FIntPoint Size = FIntPoint::ZeroValue;
		UE::UAI::Contract::FDiagnostic Error;
		bool bQueued = false;
		bool bDone = false;
	};

	FAsyncCaptureFrame::FAsyncCaptureFrame() : State(MakeShared<FState, ESPMode::ThreadSafe>()) {}
	FAsyncCaptureFrame::~FAsyncCaptureFrame()
	{
		// Commands hold State until their last use; RHI resources retire on the render thread.
		auto Keep = State;
		ENQUEUE_RENDER_COMMAND(UaiRetireReadback)([Keep](FRHICommandListImmediate&) { Keep->Readback.Reset(); });
	}
	void FAsyncCaptureFrame::Poll()
	{
		auto Keep = State;
		{
			FScopeLock Lock(&Keep->Mutex);
			if (Keep->bDone || Keep->bQueued) return;
			Keep->bQueued = true;
		}
		ENQUEUE_RENDER_COMMAND(UaiPollReadback)([Keep](FRHICommandListImmediate&)
		{
			FScopeLock Lock(&Keep->Mutex);
			Keep->bQueued = false;
			if (!Keep->Readback || !Keep->Readback->IsReady()) return;
			int32 Pitch = 0, Height = 0;
			const auto* Data = static_cast<const FColor*>(Keep->Readback->Lock(Pitch, &Height));
			if (!Data || Pitch < Keep->Size.X || Height < Keep->Size.Y)
			{
				Keep->Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("capture_async_readback_invalid"), !Data ? TEXT("The GPU readback buffer could not be locked for pixel access.") : Pitch < Keep->Size.X ? TEXT("The GPU readback row pitch is smaller than the requested image width.") : TEXT("The GPU readback buffer height is smaller than the requested image height."), TEXT("editor_capture"));
				Keep->Error.SourceKind = TEXT("native_result");
				if (Data) { Keep->Error.PropertyPath = Pitch < Keep->Size.X ? TEXT("row_pitch") : TEXT("buffer_height"); Keep->Error.ExpectedValue = MakeShared<FJsonValueNumber>(Pitch < Keep->Size.X ? Keep->Size.X : Keep->Size.Y); Keep->Error.ActualValue = MakeShared<FJsonValueNumber>(Pitch < Keep->Size.X ? Pitch : Height); }
			}
			else
			{
				Keep->Pixels.SetNumUninitialized(Keep->Size.X * Keep->Size.Y);
				for (int32 Y = 0; Y < Keep->Size.Y; ++Y)
					FMemory::Memcpy(Keep->Pixels.GetData() + Y * Keep->Size.X,
						Data + Y * Pitch, Keep->Size.X * sizeof(FColor));
			}
			if (Data) Keep->Readback->Unlock();
			Keep->Readback.Reset();
			Keep->bDone = true;
		});
	}
	bool FAsyncCaptureFrame::Take(TArray<FColor>& Pixels, FIntPoint& Size, UE::UAI::Contract::FDiagnostic& Error)
	{
		FScopeLock Lock(&State->Mutex);
		if (!State->bDone) return false;
		Pixels = MoveTemp(State->Pixels);
		Size = State->Size;
		Error = State->Error;
		return true;
	}

	struct FCaptureSurface::FState
	{
		TStrongObjectPtr<UTextureRenderTarget2D> Target;
		TWeakObjectPtr<ASceneCapture2D> Actor;
		FWidgetRenderer* Renderer = nullptr;
		void Stop()
		{
			check(IsInGameThread());
			if (Actor.IsValid())
			{
				Actor->GetCaptureComponent2D()->TextureTarget = nullptr;
				Actor->Destroy();
				Actor.Reset();
			}
		}
		~FState()
		{
			Stop();
			if (Renderer) BeginCleanup(Renderer);
		}
	};
	FCaptureSurface::FCaptureSurface() : State(MakeUnique<FState>()) {}
	FCaptureSurface::~FCaptureSurface() = default;
	void FCaptureSurface::Stop() { State->Stop(); }

	bool FCaptureSurface::Submit(TSharedPtr<FAsyncCaptureFrame, ESPMode::ThreadSafe>& Frame, UE::UAI::Contract::FDiagnostic& Error)
	{
		check(IsInGameThread());
		auto* Resource = State->Target->GameThread_GetRenderTargetResource();
		if (!Resource) { Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("capture_render_target_unavailable"), TEXT("The capture render target has no game-thread resource to submit for GPU readback."), TEXT("editor_capture")); Error.SourceKind = TEXT("native_result"); Error.ActualValue = MakeShared<FJsonValueString>(FIntPoint(State->Target->SizeX, State->Target->SizeY).ToString()); return false; }
		Frame = MakeShared<FAsyncCaptureFrame, ESPMode::ThreadSafe>();
		auto Keep = Frame->State;
		Keep->Size = FIntPoint(State->Target->SizeX, State->Target->SizeY);
		// The copy is queued immediately after this capture's render commands,
		// before the surface can be reused for another sample.
		ENQUEUE_RENDER_COMMAND(UaiSubmitReadback)([Keep, Resource](FRHICommandListImmediate& RHICmdList)
		{
			Keep->Readback = MakeUnique<FRHIGPUTextureReadback>(TEXT("UaiCapture"));
			Keep->Readback->EnqueueCopy(RHICmdList, Resource->GetRenderTargetTexture());
		});
		return true;
	}
	bool FCaptureSurface::Camera(UWorld& World, const FMinimalViewInfo& View, FIntPoint Size,
		TSharedPtr<FAsyncCaptureFrame, ESPMode::ThreadSafe>& Frame, UE::UAI::Contract::FDiagnostic& Error)
	{
		check(IsInGameThread());
		if (!State->Actor.IsValid() || State->Actor->GetWorld() != &World)
		{
			if (State->Actor.IsValid()) State->Actor->Destroy();
			FActorSpawnParameters Params;
			Params.ObjectFlags |= RF_Transient;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			State->Actor = World.SpawnActor<ASceneCapture2D>(View.Location, View.Rotation, Params);
		}
		if (!State->Actor.IsValid()) { Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("capture_camera_create_failed"), TEXT("Unreal could not spawn the temporary scene-capture camera in the requested world."), TEXT("editor_capture")); Error.CandidateName = World.GetName(); Error.SourceKind = TEXT("native_result"); return false; }
		if (!State->Target.IsValid() || State->Target->SizeX != Size.X || State->Target->SizeY != Size.Y)
		{
			State->Target.Reset(NewObject<UTextureRenderTarget2D>(GetTransientPackage(), NAME_None, RF_Transient));
			State->Target->InitCustomFormat(Size.X, Size.Y, PF_B8G8R8A8, false);
			State->Target->UpdateResourceImmediate(true);
		}
		auto* Capture = State->Actor->GetCaptureComponent2D();
		Capture->TextureTarget = State->Target.Get();
		ConfigureCameraCapture(*Capture, View);
		Capture->CaptureScene();
		return Submit(Frame, Error);
	}
	bool FCaptureSurface::PrepareWidgetTarget(FIntPoint Size, UE::UAI::Contract::FDiagnostic& Error)
	{
		check(IsInGameThread());
		if (!State->Renderer) State->Renderer = new FWidgetRenderer(false, true);
		if (!State->Target.IsValid() || State->Target->SizeX != Size.X || State->Target->SizeY != Size.Y)
			State->Target.Reset(FWidgetRenderer::CreateTargetFor(FVector2D(Size), TF_Bilinear, true));
		if (!State->Target.IsValid()) { Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("capture_widget_target_unavailable"), TEXT("The widget render target could not be created for the requested capture dimensions."), TEXT("editor_capture")); Error.PropertyPath = TEXT("capture_size"); Error.ActualValue = MakeShared<FJsonValueString>(Size.ToString()); Error.SourceKind = TEXT("native_result"); return false; }
		return true;
	}
	bool FCaptureSurface::Widget(const TSharedRef<SWidget>& Widget, FIntPoint Size, float Scale,
		TSharedPtr<FAsyncCaptureFrame, ESPMode::ThreadSafe>& Frame, UE::UAI::Contract::FDiagnostic& Error)
	{
		if (!PrepareWidgetTarget(Size, Error)) return false;
		Widget->SlatePrepass(Scale);
		State->Renderer->DrawWidget(State->Target.Get(), Widget, Scale, FVector2D(Size), 0.0f, false);
		return Submit(Frame, Error);
	}
	bool FCaptureSurface::PaintWindow(const TSharedRef<SWindow>& Window, UE::UAI::Contract::FDiagnostic& Error)
	{
		check(IsInGameThread());
		const FGeometry Geometry = Window->GetWindowGeometryInScreen();
		const FVector2D DrawSize(Geometry.GetDrawSize());
		const FIntPoint Size(FMath::CeilToInt(DrawSize.X), FMath::CeilToInt(DrawSize.Y));
		if (Size.X <= 0 || Size.Y <= 0)
		{ Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("capture_widget_window_geometry_unavailable"), TEXT("The capture window has a non-positive drawable width or height."), TEXT("editor_capture")); Error.CandidateName = Window->GetTitle().ToString(); Error.PropertyPath = TEXT("window_draw_size"); Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("positive width and height")); Error.ActualValue = MakeShared<FJsonValueString>(DrawSize.ToString()); Error.SourceKind = TEXT("native_result"); return false; }
		if (!PrepareWidgetTarget(Size, Error)) return false;
		State->Renderer->DrawWindow(State->Target.Get(), Window->GetHittestGrid(), Window,
			Geometry, Geometry.GetLayoutBoundingRect(), 0.0f, false);
		return true;
	}
}
