// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"
#include "CoreMinimal.h"
class IBlueprintEditor; class SGraphEditor; class SEditorViewport; class SWidget; class SWindow; class UBlueprint; class UEdGraph;
namespace UeAgentBlueprintEditorCapture
{
	bool PersistBlueprintGraphDocumentViewState(UBlueprint*, UEdGraph*, const FVector2f&, float);
	bool PersistBlueprintGraphDocumentViewState(UBlueprint*, UEdGraph*, const FVector2f&, float, int32*);
	bool TryGetBlueprintGraphDocumentViewState(const UBlueprint*, const UEdGraph*, FVector2f&, float&);
	bool RestoreBlueprintGraphEditorToDocumentViewState(UBlueprint*, UEdGraph*, const TSharedPtr<SGraphEditor>&, FVector2f* = nullptr, float* = nullptr);
	TSharedPtr<IBlueprintEditor> GetBlueprintEditor(UBlueprint*, bool, UE::UAI::Contract::FDiagnostic&);
	TSharedPtr<SGraphEditor> CreateOffscreenGraphEditorForCapture(UEdGraph*, const FString&, UE::UAI::Contract::FDiagnostic&);
	TSharedPtr<SWindow> ResolveBlueprintEditorWindow(const TSharedPtr<IBlueprintEditor>&, UE::UAI::Contract::FDiagnostic&);
	TSharedPtr<SEditorViewport> ResolveBlueprintViewportWidget(const TSharedPtr<IBlueprintEditor>&, UE::UAI::Contract::FDiagnostic&);
	TSharedPtr<SWidget> ResolveBlueprintViewportSceneWidget(const TSharedRef<SEditorViewport>&);
	void PrepareBlueprintViewportForCapture(const TSharedPtr<IBlueprintEditor>&, UBlueprint*, const TSharedPtr<SEditorViewport>&);
	bool ScreenshotSlateWidget(const TSharedRef<SWidget>&, TArray<FColor>&, FIntPoint&, UE::UAI::Contract::FDiagnostic&);
	bool ScreenshotSlateWidgetNonInvasive(const TSharedRef<SWidget>&, TArray<FColor>&, FIntPoint&, UE::UAI::Contract::FDiagnostic&);
	bool ScreenshotSlateWidgetNonInvasive(const TSharedRef<SWidget>&, const FVector2D&, TArray<FColor>&, FIntPoint&, UE::UAI::Contract::FDiagnostic&);
	void PrepareOffscreenGraphEditorForCapture(const TSharedPtr<SGraphEditor>&, const FVector2f&, float);
	void PrepareOffscreenGraphEditorForCapture(const TSharedPtr<SGraphEditor>&, const FVector2f&, float, int32);
	bool ScreenshotEditorViewportPixels(const TSharedRef<SEditorViewport>&, TArray<FColor>&, FIntPoint&, UE::UAI::Contract::FDiagnostic&);
	bool ScreenshotBlueprintWindow(const TSharedPtr<IBlueprintEditor>&, TArray<FColor>&, FIntPoint&, UE::UAI::Contract::FDiagnostic&);
	bool ScreenshotBlueprintWindow(const TSharedPtr<IBlueprintEditor>&, TArray<FColor>&, FIntPoint&, UE::UAI::Contract::FDiagnostic&, bool, bool*);
	bool ScreenshotBlueprintWindowCropWidget(const TSharedPtr<IBlueprintEditor>&, const TSharedRef<SWidget>&, TArray<FColor>&, FIntPoint&, UE::UAI::Contract::FDiagnostic&);
	bool ScreenshotBlueprintWindowCropWidget(const TSharedPtr<IBlueprintEditor>&, const TSharedRef<SWidget>&, TArray<FColor>&, FIntPoint&, UE::UAI::Contract::FDiagnostic&, bool, bool*);
}
