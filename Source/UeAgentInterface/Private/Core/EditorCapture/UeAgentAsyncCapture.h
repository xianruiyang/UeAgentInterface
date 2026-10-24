#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

class UWorld;
class SWidget;
class SWindow;
class USceneCaptureComponent2D;
struct FMinimalViewInfo;

namespace UE::UAI::EditorCapture
{
	/** Shared display-color camera setup for synchronous and asynchronous captures. */
	void ConfigureCameraCapture(USceneCaptureComponent2D& Capture, const FMinimalViewInfo& View, bool bCameraCut = false);

	/** One submitted BGRA frame. Rendering and polling never wait on the game thread. */
	class FAsyncCaptureFrame : public TSharedFromThis<FAsyncCaptureFrame, ESPMode::ThreadSafe>
	{
	public:
		FAsyncCaptureFrame();
		~FAsyncCaptureFrame();
		void Poll();
		bool Take(TArray<FColor>& Pixels, FIntPoint& Size, UE::UAI::Contract::FDiagnostic& Error);
	private:
		friend class FCaptureSurface;
		struct FState;
		TSharedRef<FState, ESPMode::ThreadSafe> State;
	};

	/** Game-thread owner of a reusable camera or Slate capture surface. */
	class FCaptureSurface
	{
	public:
		FCaptureSurface();
		~FCaptureSurface();
		// Remove the transient scene actor while retaining submitted render resources.
		void Stop();
		bool Camera(UWorld& World, const FMinimalViewInfo& View, FIntPoint Size,
			TSharedPtr<FAsyncCaptureFrame, ESPMode::ThreadSafe>& Frame, UE::UAI::Contract::FDiagnostic& Error);
		bool Widget(const TSharedRef<SWidget>& Widget, FIntPoint Size, float Scale,
			TSharedPtr<FAsyncCaptureFrame, ESPMode::ThreadSafe>& Frame, UE::UAI::Contract::FDiagnostic& Error);
		// Paint the mounted window into its own native hit-test grid. No pixel readback,
		// widget reparenting, Slate tick or physical-window activation is required.
		bool PaintWindow(const TSharedRef<SWindow>& Window, UE::UAI::Contract::FDiagnostic& Error);
	private:
		struct FState;
		TUniquePtr<FState> State;
		bool Submit(TSharedPtr<FAsyncCaptureFrame, ESPMode::ThreadSafe>& Frame, UE::UAI::Contract::FDiagnostic& Error);
		bool PrepareWidgetTarget(FIntPoint Size, UE::UAI::Contract::FDiagnostic& Error);
	};
}
