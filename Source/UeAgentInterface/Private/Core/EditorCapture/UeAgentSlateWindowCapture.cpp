#include "Core/EditorCapture/UeAgentSlateWindowCapture.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Dom/JsonValue.h"
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
		const bool bRequireNativeSize, const bool bSkipIfReady)
	{
		const auto Widget = Viewport ? Viewport->GetViewportWidget().Pin() : nullptr;
		const auto Window = Widget && FSlateApplication::IsInitialized()
			? FSlateApplication::Get().FindWidgetWindow(Widget.ToSharedRef()) : nullptr;
		auto HasExtent = [&]() { const auto Size = Viewport->GetSizeXY(); return Size.X > 0 && Size.Y > 0; };
		// Level captures may revisit this viewport for each sample. Once ready,
		// do not add window activation, Slate ticks or draws to the sampling cadence.
		if (bSkipIfReady && Window && Window->IsVisible() && !Window->IsWindowMinimized()
			&& (!bRequireNativeSize || HasExtent())) return true;
		if (!Prepare(Window, OutError)) return false;
		if (!bRequireNativeSize || HasExtent()) return true;

		// A persisted floating toolkit window can be smaller than its toolbar,
		// leaving every native viewport at zero height. Recover only this invalid
		// layout using the user's main editor extent, without changing viewport
		// fixed-size policy, authored capture dimensions, or saved layout data.
		const auto RootWindow = FGlobalTabmanager::Get()->GetRootWindow();
		if (RootWindow && RootWindow != Window)
		{
			const FVector2D RootSize = RootWindow->GetClientSizeInScreen();
			const FVector2D CurrentSize = Window->GetClientSizeInScreen();
			if (RootSize.X > 0 && RootSize.Y > 0 && (RootSize.X > CurrentSize.X || RootSize.Y > CurrentSize.Y))
			{
				Window->Resize(FVector2D(FMath::Max(RootSize.X, CurrentSize.X), FMath::Max(RootSize.Y, CurrentSize.Y)));
				if (!Prepare(Window, OutError)) return false;
				if (HasExtent()) return true;
			}
		}
		OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("invalid_viewport_size"), TEXT("The native viewport still has a non-positive width or height after its capture window was prepared."), TEXT("editor_capture"));
		OutError.CandidateName = Window->GetTitle().ToString(); OutError.SourceKind = TEXT("native_result"); OutError.PropertyPath = TEXT("viewport_size");
		OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("positive width and height")); OutError.ActualValue = MakeShared<FJsonValueString>(Viewport->GetSizeXY().ToString());
		return false;
	}
}
