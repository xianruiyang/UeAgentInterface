// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"
#include "CoreMinimal.h"
class SWidget;

namespace UeAgentSlateWidgetCapture {
// Capture the widget in its existing native window and current layout.
bool CaptureLiveWidgetPixels(const TSharedRef<SWidget>& Widget,
    TArray<FColor>& OutPixels, FIntPoint& OutSize, UE::UAI::Contract::FDiagnostic& OutError);
// Draw an existing Slate tree. Scene/UI selection and output alpha policy stay
// with the caller; this owner handles render-target lifetime and color encoding.
bool CaptureWidgetPixels(const TSharedRef<SWidget>& Widget,
    const FVector2D& DrawSize, float DrawScale, TArray<FColor>& OutPixels,
    FIntPoint& OutSize, UE::UAI::Contract::FDiagnostic& OutError);
}
