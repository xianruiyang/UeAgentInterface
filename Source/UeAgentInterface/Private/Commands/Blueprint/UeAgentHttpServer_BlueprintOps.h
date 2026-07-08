// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraphPin.h"
#include "Server/UeAgentHttpServerTypes.h"
#include "Engine/Blueprint.h"
#include "Kismet2/CompilerResultsLog.h"

class FJsonObject;
class FJsonValue;
class IBlueprintEditor;
class SGraphEditor;
class SEditorViewport;
class SWidget;
class SWindow;
class UClass;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;
class USCS_Node;
class USimpleConstructionScript;
class FProperty;
class UObject;

namespace UeAgentBlueprintOps
{
	enum class ECompilerMessageSeverityFilter : uint8
	{
		All,
		Error,
		Warning,
		Info,
		WarningOrError
	};

	FString NormalizeAssetPath(const FString& InPath);
	bool AssetExists(const FString& InPath);
	UObject* LoadAssetObject(const FString& InPath);
	UBlueprint* LoadBlueprintAsset(const FString& InPath);
	UClass* ResolveClassByPath(const FString& InClassPath);
	FString BlueprintStatusToString(EBlueprintStatus InStatus);
	bool ParseCompilerSeverityFilter(const FString& InFilterText, ECompilerMessageSeverityFilter& OutFilter);
	void BuildCompilerLogJson(
		const FCompilerResultsLog& InCompilerLog,
		ECompilerMessageSeverityFilter InFilter,
		int32 InMaxMessages,
		TArray<TSharedPtr<FJsonValue>>& OutMessages,
		int32& OutFilteredErrorCount,
		int32& OutFilteredWarningCount,
		int32& OutFilteredInfoCount);
	UEdGraph* ResolveBlueprintGraph(UBlueprint* Blueprint, const FString& InGraphName);
	bool SaveBlueprintPackage(UBlueprint* Blueprint, FString& OutError);

	bool PersistBlueprintGraphDocumentViewState(UBlueprint* Blueprint, UEdGraph* Graph, const FVector2f& ViewLocation, float ZoomAmount);
	bool PersistBlueprintGraphDocumentViewState(UBlueprint* Blueprint, UEdGraph* Graph, const FVector2f& ViewLocation, float ZoomAmount, int32* OutRemovedExistingEntryCount);
	bool TryGetBlueprintGraphDocumentViewState(const UBlueprint* Blueprint, const UEdGraph* Graph, FVector2f& OutViewLocation, float& OutZoomAmount);
	bool RestoreBlueprintGraphEditorToDocumentViewState(UBlueprint* Blueprint, UEdGraph* Graph, const TSharedPtr<SGraphEditor>& GraphEditor, FVector2f* OutRestoredViewLocation, float* OutRestoredZoomAmount);
	TSharedPtr<IBlueprintEditor> GetBlueprintEditor(UBlueprint* Blueprint, bool bOpenIfNeeded, FString& OutError);
	TSharedPtr<SGraphEditor> CreateOffscreenGraphEditorForCapture(UEdGraph* Graph, const FString& CornerText, FString& OutError);
	TSharedPtr<SWindow> ResolveBlueprintEditorWindow(const TSharedPtr<IBlueprintEditor>& BlueprintEditor, FString& OutError);
	TSharedPtr<SEditorViewport> ResolveBlueprintViewportWidget(const TSharedPtr<IBlueprintEditor>& BlueprintEditor, FString& OutError);
	TSharedPtr<SWidget> ResolveBlueprintViewportSceneWidget(const TSharedRef<SEditorViewport>& ViewportWidget);
	void PrepareBlueprintViewportForCapture(const TSharedPtr<IBlueprintEditor>& BlueprintEditor, UBlueprint* Blueprint, const TSharedPtr<SEditorViewport>& ViewportWidget);
	void PumpAndTickSlateForCapture();
	bool PrepareSlateWindowForCapture(const TSharedPtr<SWindow>& Window, FString& OutError);
	bool PrepareSlateWindowForCapture(const TSharedPtr<SWindow>& Window, FString& OutError, bool bAllowWindowMutation, bool* OutWindowMutated);
	bool ScreenshotSlateWidget(const TSharedRef<SWidget>& Widget, TArray<FColor>& OutPixels, FIntPoint& OutSize, FString& OutError);
	bool ScreenshotSlateWidgetNonInvasive(const TSharedRef<SWidget>& Widget, TArray<FColor>& OutPixels, FIntPoint& OutSize, FString& OutError);
	bool ScreenshotSlateWidgetNonInvasive(const TSharedRef<SWidget>& Widget, const FVector2D& RequestedDrawSize, TArray<FColor>& OutPixels, FIntPoint& OutSize, FString& OutError);
	void PrepareOffscreenGraphEditorForCapture(const TSharedPtr<SGraphEditor>& GraphEditor, const FVector2f& ViewLocation, float ZoomAmount);
	void PrepareOffscreenGraphEditorForCapture(const TSharedPtr<SGraphEditor>& GraphEditor, const FVector2f& ViewLocation, float ZoomAmount, int32 FrameCount);
	bool ScreenshotEditorViewportPixels(const TSharedRef<SEditorViewport>& ViewportWidget, TArray<FColor>& OutPixels, FIntPoint& OutSize, FString& OutError);
	bool ScreenshotBlueprintWindow(const TSharedPtr<IBlueprintEditor>& BlueprintEditor, TArray<FColor>& OutPixels, FIntPoint& OutSize, FString& OutError);
	bool ScreenshotBlueprintWindow(const TSharedPtr<IBlueprintEditor>& BlueprintEditor, TArray<FColor>& OutPixels, FIntPoint& OutSize, FString& OutError, bool bAllowWindowMutation, bool* OutWindowMutated);
	bool ScreenshotBlueprintWindowCropWidget(const TSharedPtr<IBlueprintEditor>& BlueprintEditor, const TSharedRef<SWidget>& Widget, TArray<FColor>& OutPixels, FIntPoint& OutSize, FString& OutError);
	bool ScreenshotBlueprintWindowCropWidget(const TSharedPtr<IBlueprintEditor>& BlueprintEditor, const TSharedRef<SWidget>& Widget, TArray<FColor>& OutPixels, FIntPoint& OutSize, FString& OutError, bool bAllowWindowMutation, bool* OutWindowMutated);
	USCS_Node* FindSCSNodeByAnyName(USimpleConstructionScript* SCS, const FString& InComponentName);
	UEdGraphNode* FindGraphNodeByGuid(UEdGraph* Graph, const FString& InNodeGuid);
	UEdGraphPin* FindNodePinByName(UEdGraphNode* Node, const FString& InPinName);
	bool ResolvePropertyPath(UObject* Obj, const FString& PropertyPath, FProperty*& OutProperty, void*& OutValuePtr);
	bool IsUserConstructionScriptGraph(const UEdGraph* Graph);
	bool IsUnsafeConstructionScriptPcgCall(const FString& FunctionOwnerClass, const FString& FunctionName);
	bool RequiresExplicitParentOrEventNode(const FString& FunctionName);
	FString PinContainerTypeToString(EPinContainerType InContainerType);
	bool BuildPinTypeFromJsonObject(const TSharedPtr<FJsonObject>& TypeObj, FEdGraphPinType& OutPinType, FString& OutError);

	bool ExecuteBlueprintLayoutAllGraphsCommand(
		const FUeAgentRequestContext& Ctx,
		TSharedPtr<FJsonObject>& OutData,
		FString& OutError,
		TFunctionRef<bool(const FUeAgentRequestContext&, TSharedPtr<FJsonObject>&, FString&)> RunGraphLayout);
}
