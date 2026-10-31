#include "Core/EditorViewport/UeAgentViewportRenderSize.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "Slate/SceneViewport.h"
#include "Framework/Application/SlateApplication.h"
#include "GenericPlatform/GenericApplication.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"

namespace UE::UAI::EditorViewport
{
	FIntPoint ResolveZeroViewportExtent(const TSharedPtr<FSceneViewport>& Viewport, FString& OutSource)
	{
		FVector2D DisplaySize = FVector2D::ZeroVector;
		if (Viewport && FSlateApplication::IsInitialized())
		{
			if (const auto Widget = Viewport->GetViewportWidget().Pin())
			{
				if (const auto Window = FSlateApplication::Get().FindWidgetWindow(Widget.ToSharedRef()))
				{
					// Restored geometry keeps monitor identity while the OS window is minimized.
					const auto WorkArea = FSlateApplication::Get().GetWorkArea(Window->GetNonMaximizedRectInScreen());
					DisplaySize = FVector2D(WorkArea.Right - WorkArea.Left, WorkArea.Bottom - WorkArea.Top);
					OutSource = TEXT("zero_extent_window_display_work_area");
				}
			}
		}
		if (!FMath::IsFinite(DisplaySize.X) || !FMath::IsFinite(DisplaySize.Y) || DisplaySize.X <= 0 || DisplaySize.Y <= 0)
		{
			FDisplayMetrics Metrics;
			FDisplayMetrics::RebuildDisplayMetrics(Metrics);
			const auto& WorkArea = Metrics.PrimaryDisplayWorkAreaRect;
			DisplaySize = FVector2D(WorkArea.Right - WorkArea.Left, WorkArea.Bottom - WorkArea.Top);
			OutSource = TEXT("zero_extent_primary_display_work_area");
		}
		if (!FMath::IsFinite(DisplaySize.X) || !FMath::IsFinite(DisplaySize.Y) || DisplaySize.X <= 0 || DisplaySize.Y <= 0)
		{
			OutSource = TEXT("zero_extent_fallback_1280x720");
			return FIntPoint(1280, 720);
		}
		// Match the existing Level cold-start bound while retaining the monitor aspect.
		const double Scale = FMath::Min(1., 4096. / FMath::Max(DisplaySize.X, DisplaySize.Y));
		return FIntPoint(FMath::Max(1, FMath::RoundToInt(DisplaySize.X * Scale)),
			FMath::Max(1, FMath::RoundToInt(DisplaySize.Y * Scale)));
	}

	FScopedRenderSize::~FScopedRenderSize() { Restore(); }

	bool FScopedRenderSize::Apply(const TSharedPtr<FSceneViewport>& Viewport, FIntPoint Requested, UE::UAI::Contract::FDiagnostic& OutError)
	{
		Restore();
		bChanged = false;
		bRestored = true;
		ExtentSource = TEXT("existing_viewport");
		const auto SizeValue = [](FIntPoint Size) -> TSharedPtr<FJsonValue> {
			auto Value = MakeShared<FJsonObject>();
			Value->SetNumberField(TEXT("render_width"), Size.X); Value->SetNumberField(TEXT("render_height"), Size.Y);
			return MakeShared<FJsonValueObject>(Value);
		};
		if (!Viewport) {
			if (Requested == FIntPoint::ZeroValue) return true; // Widget previews have no scene viewport.
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("preview_render_size_unavailable"), TEXT("The preview has no native viewport on which to apply the requested render size."), TEXT("viewport_render_size"));
			OutError.SourceKind = TEXT("native_result"); OutError.CandidateName = TEXT("preview viewport");
			OutError.ExpectedValue = SizeValue(Requested); OutError.ActualValuePresent = false; return false;
		}
		if (Requested.X < 0 || Requested.Y < 0) {
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("preview_render_size_unavailable"), TEXT("A requested render dimension must be non-negative; zero infers that axis from the viewport aspect ratio."), TEXT("viewport_render_size"));
			OutError.PropertyPath = Requested.X < 0 ? TEXT("render_width") : TEXT("render_height");
			OutError.Source.JsonPath = TEXT("/") + OutError.PropertyPath; OutError.SemanticPointer = OutError.Source.JsonPath;
			OutError.SourceKind = TEXT("command_input");
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-negative integer"));
			OutError.ActualValue = MakeShared<FJsonValueNumber>(Requested.X < 0 ? Requested.X : Requested.Y); return false;
		}
		OriginalSize = Viewport->GetSizeXY();
		RenderSize = OriginalSize;
		bOriginallyFixed = Viewport->HasFixedSize();
		if (Requested == FIntPoint::ZeroValue && OriginalSize.X > 0 && OriginalSize.Y > 0) return true;
		FIntPoint AspectSize = OriginalSize;
		ExtentSource = TEXT("explicit_render_size");
		// A fully specified extent can initialize a window-bound native viewport
		// before its first Slate layout. Only an inferred axis needs the old aspect.
		if ((!Requested.X || !Requested.Y) && (OriginalSize.X <= 0 || OriginalSize.Y <= 0))
		{
			AspectSize = ResolveZeroViewportExtent(Viewport, ExtentSource);
			if (Requested == FIntPoint::ZeroValue) Requested = AspectSize;
		}
		if (!Requested.X) Requested.X = static_cast<int32>(FMath::Clamp(
			FMath::RoundToDouble(static_cast<double>(Requested.Y) * AspectSize.X / AspectSize.Y), 1., static_cast<double>(MAX_int32)));
		if (!Requested.Y) Requested.Y = static_cast<int32>(FMath::Clamp(
			FMath::RoundToDouble(static_cast<double>(Requested.X) * AspectSize.Y / AspectSize.X), 1., static_cast<double>(MAX_int32)));
		NativeViewport = Viewport;
		bOriginallyFixed = Viewport->HasFixedSize();
		bApplied = true;
		bChanged = true;
		bRestored = false;
		Viewport->SetFixedViewportSize(Requested.X, Requested.Y);
		RenderSize = Viewport->GetSizeXY();
		if (Viewport->GetSizeXY() != Requested)
		{
			const FIntPoint ActualSize = Viewport->GetSizeXY();
			Restore();
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("preview_render_size_unavailable"), TEXT("The native viewport did not adopt the requested fixed render size."), TEXT("viewport_render_size"));
			OutError.SourceKind = TEXT("native_result"); OutError.CandidateName = TEXT("preview viewport");
			OutError.ExpectedValue = SizeValue(Requested); OutError.ActualValue = SizeValue(ActualSize);
			return false;
		}
		return true;
	}

	void FScopedRenderSize::Restore()
	{
		if (!bApplied) return;
		if (const auto Viewport = NativeViewport.Pin())
		{
			// Restore a valid old RHI extent, then return geometry ownership to Slate.
			// An initially unlaid viewport has no old extent; its next paint supplies it.
			if (OriginalSize.X > 0 && OriginalSize.Y > 0)
				Viewport->SetFixedViewportSize(OriginalSize.X, OriginalSize.Y);
			if (!bOriginallyFixed) Viewport->SetFixedViewportSize(0, 0);
			bRestored = Viewport->HasFixedSize() == bOriginallyFixed
				&& (OriginalSize.X <= 0 || OriginalSize.Y <= 0 || Viewport->GetSizeXY() == OriginalSize);
		}
		bApplied = false;
		NativeViewport.Reset();
	}

	TSharedPtr<FJsonObject> FScopedRenderSize::Describe() const
	{
		const auto Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("performed"), bChanged);
		Result->SetStringField(TEXT("extent_source"), ExtentSource);
		if (bChanged) Result->SetStringField(TEXT("reason"), OriginalSize.X <= 0 || OriginalSize.Y <= 0 ? TEXT("zero_native_extent") : TEXT("explicit_render_size"));
		Result->SetNumberField(TEXT("native_width_before"), OriginalSize.X);
		Result->SetNumberField(TEXT("native_height_before"), OriginalSize.Y);
		Result->SetNumberField(TEXT("render_width"), RenderSize.X);
		Result->SetNumberField(TEXT("render_height"), RenderSize.Y);
		Result->SetBoolField(TEXT("fixed_size_before"), bOriginallyFixed);
		Result->SetBoolField(TEXT("fixed_size_restored"), bRestored);
		return Result;
	}
}
