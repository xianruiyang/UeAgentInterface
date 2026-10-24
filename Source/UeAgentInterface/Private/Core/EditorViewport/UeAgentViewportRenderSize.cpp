#include "Core/EditorViewport/UeAgentViewportRenderSize.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "Slate/SceneViewport.h"

namespace UE::UAI::EditorViewport
{
	FScopedRenderSize::~FScopedRenderSize() { Restore(); }

	bool FScopedRenderSize::Apply(const TSharedPtr<FSceneViewport>& Viewport, FIntPoint Requested, UE::UAI::Contract::FDiagnostic& OutError)
	{
		Restore();
		if (Requested == FIntPoint::ZeroValue) return true;
		const auto SizeValue = [](FIntPoint Size) -> TSharedPtr<FJsonValue> {
			auto Value = MakeShared<FJsonObject>();
			Value->SetNumberField(TEXT("render_width"), Size.X); Value->SetNumberField(TEXT("render_height"), Size.Y);
			return MakeShared<FJsonValueObject>(Value);
		};
		if (!Viewport) {
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
		// A fully specified extent can initialize a window-bound native viewport
		// before its first Slate layout. Only an inferred axis needs the old aspect.
		if ((!Requested.X || !Requested.Y) && (OriginalSize.X <= 0 || OriginalSize.Y <= 0))
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("invalid_viewport_size"), TEXT("A render dimension was left at zero, but the viewport has no positive width and height from which to infer the aspect ratio."), TEXT("viewport_render_size"));
			OutError.SourceKind = TEXT("native_result"); OutError.CandidateName = TEXT("preview viewport");
			OutError.PropertyPath = !Requested.X ? TEXT("render_width") : TEXT("render_height");
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("positive native viewport width and height")); OutError.ActualValue = SizeValue(OriginalSize);
			return false;
		}
		if (!Requested.X) Requested.X = static_cast<int32>(FMath::Clamp(
			FMath::RoundToDouble(static_cast<double>(Requested.Y) * OriginalSize.X / OriginalSize.Y), 1., static_cast<double>(MAX_int32)));
		if (!Requested.Y) Requested.Y = static_cast<int32>(FMath::Clamp(
			FMath::RoundToDouble(static_cast<double>(Requested.X) * OriginalSize.Y / OriginalSize.X), 1., static_cast<double>(MAX_int32)));
		NativeViewport = Viewport;
		bOriginallyFixed = Viewport->HasFixedSize();
		bApplied = true;
		Viewport->SetFixedViewportSize(Requested.X, Requested.Y);
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
		}
		bApplied = false;
		NativeViewport.Reset();
	}
}
