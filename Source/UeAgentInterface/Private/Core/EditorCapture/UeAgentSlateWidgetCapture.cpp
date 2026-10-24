// Copyright Epic Games, Inc. All Rights Reserved.
#include "Core/EditorCapture/UeAgentSlateWidgetCapture.h"
#include "Dom/JsonValue.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Framework/Application/SlateApplication.h"
#include "Layout/WidgetPath.h"
#include "RenderDeferredCleanup.h"
#include "RenderingThread.h"
#include "TextureResource.h"
#include "Slate/WidgetRenderer.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/SWidget.h"

namespace UeAgentSlateWidgetCapture {
bool CaptureLiveWidgetPixels(const TSharedRef<SWidget>& Widget,
    TArray<FColor>& OutPixels, FIntPoint& OutSize, UE::UAI::Contract::FDiagnostic& OutError) {
  FWidgetPath Path;
  if (!FSlateApplication::IsInitialized() ||
      !FSlateApplication::Get().GeneratePathToWidgetUnchecked(Widget, Path)) {
    OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("live_widget_layout_unavailable"), !FSlateApplication::IsInitialized() ? TEXT("Slate is not initialized, so the live widget cannot be located.") : TEXT("The requested widget is not part of a live Slate widget path."), TEXT("editor_capture"));
    OutError.CandidateName = Widget->GetTypeAsString(); OutError.SourceKind = TEXT("native_result");
    return false;
  }
  FIntVector Size;
  OutPixels.Reset();
  if (!FSlateApplication::Get().TakeScreenshot(Widget, OutPixels, Size)) {
    OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("live_widget_capture_failed"), TEXT("Slate could not capture pixels from the requested live widget."), TEXT("editor_capture")); OutError.CandidateName = Widget->GetTypeAsString(); OutError.SourceKind = TEXT("native_result");
    return false;
  }
  OutSize = FIntPoint(Size.X, Size.Y);
  for (FColor& Pixel : OutPixels) Pixel.A = 255;
  return true;
}

bool CaptureWidgetPixels(const TSharedRef<SWidget> &Widget,
                                         const FVector2D &DrawSize,
                                         const float DrawScale,
                                         TArray<FColor> &OutPixels,
                                         FIntPoint &OutSize,
                                         UE::UAI::Contract::FDiagnostic& OutError) {
  if (!FSlateApplication::IsInitialized()) {
    OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("slate_not_initialized"), TEXT("Slate is not initialized, so an offscreen widget cannot be rendered."), TEXT("editor_capture")); OutError.CandidateName = Widget->GetTypeAsString(); OutError.SourceKind = TEXT("native_result");
    return false;
  }

  // CreateTargetFor below creates an sRGB render target. Let that target encode
  // linear Slate output once; shader gamma correction would encode it again.
  FWidgetRenderer *WidgetRenderer = new FWidgetRenderer(false, true);
  if (!WidgetRenderer) {
    OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("widget_renderer_create_failed"), TEXT("The offscreen widget renderer could not be created."), TEXT("editor_capture")); OutError.CandidateName = Widget->GetTypeAsString(); OutError.SourceKind = TEXT("native_result");
    return false;
  }

  UTextureRenderTarget2D *RenderTarget =
      FWidgetRenderer::CreateTargetFor(DrawSize, TF_Bilinear, true);
  if (!RenderTarget) {
    BeginCleanup(WidgetRenderer);
    OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("offscreen_render_target_create_failed"), TEXT("The offscreen widget render target could not be created for the requested draw size."), TEXT("editor_capture")); OutError.CandidateName = Widget->GetTypeAsString(); OutError.PropertyPath = TEXT("draw_size"); OutError.ActualValue = MakeShared<FJsonValueString>(DrawSize.ToString()); OutError.SourceKind = TEXT("native_result");
    return false;
  }

  TStrongObjectPtr<UTextureRenderTarget2D> RenderTargetGuard(RenderTarget);
  Widget->SlatePrepass(DrawScale);
  WidgetRenderer->DrawWidget(RenderTarget, Widget, DrawScale, DrawSize, 0.0f, false);
  FlushRenderingCommands();
  BeginCleanup(WidgetRenderer);

  FRenderTarget *RenderTargetResource =
      RenderTarget->GameThread_GetRenderTargetResource();
  if (!RenderTargetResource) {
    OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("offscreen_render_target_resource_missing"), TEXT("The offscreen widget render target has no game-thread resource after rendering."), TEXT("editor_capture")); OutError.CandidateName = Widget->GetTypeAsString(); OutError.SourceKind = TEXT("native_result");
    return false;
  }

  FReadSurfaceDataFlags ReadFlags(RCM_UNorm);
  ReadFlags.SetLinearToGamma(false);
  OutPixels.Reset();
  const bool bReadPixels = RenderTargetResource->ReadPixels(OutPixels, ReadFlags);
  if (!bReadPixels || OutPixels.Num() <= 0) {
    OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("offscreen_read_pixels_failed"), !bReadPixels ? TEXT("Reading pixels from the offscreen widget render target failed.") : TEXT("The offscreen widget render target returned an empty pixel buffer."), TEXT("editor_capture")); OutError.CandidateName = Widget->GetTypeAsString(); OutError.ActualValue = MakeShared<FJsonValueNumber>(OutPixels.Num()); OutError.SourceKind = TEXT("native_result");
    return false;
  }

  OutSize = FIntPoint(RenderTarget->SizeX, RenderTarget->SizeY);
  if (OutSize.X <= 0 || OutSize.Y <= 0 ||
      OutPixels.Num() != static_cast<int64>(OutSize.X) * OutSize.Y) {
    OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("offscreen_screenshot_empty"), OutSize.X <= 0 || OutSize.Y <= 0 ? TEXT("The offscreen widget render target has a non-positive width or height.") : TEXT("The offscreen widget pixel count does not match the render target dimensions."), TEXT("editor_capture"));
    OutError.CandidateName = Widget->GetTypeAsString(); OutError.SourceKind = TEXT("native_result");
    if (OutSize.X <= 0 || OutSize.Y <= 0) { OutError.PropertyPath = TEXT("render_size"); OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("positive width and height")); OutError.ActualValue = MakeShared<FJsonValueString>(OutSize.ToString()); }
    else { OutError.PropertyPath = TEXT("pixel_count"); OutError.ExpectedValue = MakeShared<FJsonValueNumber>(static_cast<int64>(OutSize.X) * OutSize.Y); OutError.ActualValue = MakeShared<FJsonValueNumber>(OutPixels.Num()); }
    return false;
  }
  return true;
}

}
