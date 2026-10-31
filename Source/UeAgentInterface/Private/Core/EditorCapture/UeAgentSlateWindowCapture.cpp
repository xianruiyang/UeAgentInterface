#include "Core/EditorCapture/UeAgentSlateWindowCapture.h"

#include "Framework/Application/SlateApplication.h"
#include "Dom/JsonValue.h"
#include "Dom/JsonObject.h"
#include "Slate/SceneViewport.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"

namespace UE::UAI::EditorCapture::SlateWindow
{
	void PumpAndTick()
	{
		if (!FSlateApplication::IsInitialized()) return;
		auto& Slate = FSlateApplication::Get();
		Slate.PumpMessages();
		if (!Slate.IsTicking()) Slate.Tick(ESlateTickType::All);
	}

	bool Prepare(const TSharedPtr<SWindow>& Window, UE::UAI::Contract::FDiagnostic& OutError,
		const bool bAllowWindowMutation, bool* OutWindowMutated)
	{
		if (OutWindowMutated) *OutWindowMutated = false;
		if (!Window.IsValid() || !FSlateApplication::IsInitialized())
		{ OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("slate_window_not_available"), !Window.IsValid() ? TEXT("The window required for capture is unavailable.") : TEXT("Slate is not initialized, so the capture window cannot be prepared."), TEXT("editor_capture")); OutError.SourceKind = TEXT("native_result"); return false; }
		if (Window->IsWindowMinimized())
		{
			if (!bAllowWindowMutation)
			{ OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("slate_window_minimized_ui_mutation_disabled"), TEXT("The capture window is minimized and this request does not allow restoring it."), TEXT("editor_capture")); OutError.CandidateName = Window->GetTitle().ToString(); OutError.SourceKind = TEXT("native_result"); return false; }
			Window->Restore();
			if (OutWindowMutated) *OutWindowMutated = true;
			FSlateApplication::Get().PumpMessages();
		}
		if (!Window->IsVisible())
		{
			if (!bAllowWindowMutation)
			{ OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("slate_window_hidden_ui_mutation_disabled"), TEXT("The capture window is hidden and this request does not allow showing it."), TEXT("editor_capture")); OutError.CandidateName = Window->GetTitle().ToString(); OutError.SourceKind = TEXT("native_result"); return false; }
			Window->ShowWindow();
			if (OutWindowMutated) *OutWindowMutated = true;
		}
		if (bAllowWindowMutation)
		{
			Window->BringToFront(true);
			if (OutWindowMutated) *OutWindowMutated = true;
			FSlateApplication::Get().PumpMessages();
		}
		if (!Window->IsVisible() || Window->IsWindowMinimized())
		{ OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("slate_window_not_renderable"), !Window->IsVisible() ? TEXT("The capture window is still hidden after preparation.") : TEXT("The capture window is still minimized after preparation."), TEXT("editor_capture")); OutError.CandidateName = Window->GetTitle().ToString(); OutError.SourceKind = TEXT("native_result"); return false; }
		if (!bAllowWindowMutation) return true;
		// Native layout/paint establishes the viewport extent once its host gives
		// the viewport a drawable area.
		for (int32 Frame = 0; Frame < 3; ++Frame)
		{
			PumpAndTick();
			auto& Slate = FSlateApplication::Get();
			Window->SlatePrepass(Slate.GetApplicationScale() * Window->GetDPIScaleFactor());
			Slate.ForceRedrawWindow(Window.ToSharedRef());
		}
		PumpAndTick();
		return true;
	}

	bool PrepareViewport(const TSharedPtr<FSceneViewport>& Viewport, UE::UAI::Contract::FDiagnostic& OutError,
		const bool bRequireNativeSize)
	{
		if (!Viewport)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("preview_viewport_unavailable"), TEXT("The existing editor viewport is unavailable for capture."), TEXT("editor_capture"));
			OutError.SourceKind = TEXT("native_result");
			return false;
		}
		// Viewport drawing/readback uses the existing render target, not desktop
		// presentation. A minimized host can retain a usable native extent. Never
		// restore, show, activate or resize the user's window to take a screenshot.
		const FIntPoint Size = Viewport->GetSizeXY();
		if (!bRequireNativeSize || (Size.X > 0 && Size.Y > 0)) return true;
		OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("invalid_viewport_size"), TEXT("The existing viewport has no drawable extent and this capture path has not initialized one; capture preserves its host window state."), TEXT("editor_capture"));
		OutError.SourceKind = TEXT("native_result"); OutError.PropertyPath = TEXT("viewport_size");
		OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("positive width and height")); OutError.ActualValue = MakeShared<FJsonValueString>(Viewport->GetSizeXY().ToString());
		return false;
	}

	FScopedViewportExtent::~FScopedViewportExtent()
	{
		Restore();
	}

	bool FScopedViewportExtent::Prepare(const TSharedPtr<FSceneViewport>& InViewport,
		const FIntPoint* ColdStartSize, const FString& ColdStartSource, UE::UAI::Contract::FDiagnostic& OutError)
	{
		Viewport = InViewport;
		if (!Viewport) return PrepareViewport(Viewport, OutError);
		ExtentBefore = Viewport->GetSizeXY();
		bFixedBefore = Viewport->HasFixedSize();
		if ((ExtentBefore.X > 0 && ExtentBefore.Y > 0) || !ColdStartSize)
			return PrepareViewport(Viewport, OutError);
		if (bFixedBefore || ColdStartSize->X <= 0 || ColdStartSize->Y <= 0)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("viewport_initialization_invalid"),
				TEXT("Cold-start initialization requires a layout-driven viewport and a positive render extent."), TEXT("editor_capture"));
			OutError.SourceKind = TEXT("native_result");
			return false;
		}
		RequestedExtent = *ColdStartSize;
		ExtentSource = ColdStartSource;
		// This native API resizes this FSceneViewport's RHI resources, not its OS
		// window. The temporary fixed flag prevents zero Slate allotment from
		// replacing the requested render extent during capture.
		bTemporaryFixed = true;
		Viewport->SetFixedViewportSize(RequestedExtent.X, RequestedExtent.Y);
		const FIntPoint Actual = Viewport->GetSizeXY();
		if (Actual != RequestedExtent)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("viewport_initialization_failed"),
				TEXT("The existing editor viewport did not create the requested cold-start render extent."), TEXT("editor_capture"));
			OutError.SourceKind = TEXT("native_result"); OutError.PropertyPath = TEXT("cold_start_render_size");
			OutError.ExpectedValue = MakeShared<FJsonValueString>(RequestedExtent.ToString());
			OutError.ActualValue = MakeShared<FJsonValueString>(Actual.ToString());
			return false;
		}
		bInitialized = true;
		return true;
	}

	void FScopedViewportExtent::Restore()
	{
		if (bTemporaryFixed && Viewport)
		{
			// Keep the initialized resource; normal Slate layout may resize it on
			// the next paint. No fixed-size policy or persisted layout survives.
			Viewport->SetFixedViewportSize(0, 0);
			bTemporaryFixed = false;
		}
	}

	TSharedPtr<FJsonObject> FScopedViewportExtent::Describe() const
	{
		const auto Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("performed"), bInitialized);
		if (bInitialized) Result->SetStringField(TEXT("reason"), TEXT("zero_native_extent"));
		Result->SetStringField(TEXT("extent_source"), ExtentSource);
		Result->SetNumberField(TEXT("native_width_before"), ExtentBefore.X);
		Result->SetNumberField(TEXT("native_height_before"), ExtentBefore.Y);
		const FIntPoint After = Viewport ? Viewport->GetSizeXY() : FIntPoint::ZeroValue;
		Result->SetNumberField(TEXT("render_width"), After.X);
		Result->SetNumberField(TEXT("render_height"), After.Y);
		Result->SetBoolField(TEXT("fixed_size_before"), bFixedBefore);
		Result->SetBoolField(TEXT("fixed_size_restored"), Viewport && Viewport->HasFixedSize() == bFixedBefore);
		return Result;
	}
}
