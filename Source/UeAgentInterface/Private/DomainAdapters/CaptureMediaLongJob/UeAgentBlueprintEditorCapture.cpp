#include "DomainAdapters/CaptureMediaLongJob/UeAgentBlueprintEditorCapture.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureMediaDiagnostics.h"
#include "Misc/ScopeExit.h"

#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringOps.h"
#include "Core/EditorCapture/Blueprint/UeAgentBlueprintEditorCaptureMechanics.h"
#include "Core/EditorCapture/UeAgentSlateWindowCapture.h"
#include "Core/GraphLayout/UeAgentGraphViewUtils.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureImageProcessing.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentViewportCapture.h"
#include "Module/UeAgentInterfaceLogger.h"

#include "BlueprintEditor.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "GraphEditor.h"
#include "HAL/FileManager.h"
#include "SEditorViewport.h"
#include "Widgets/SWindow.h"
#include "Widgets/SWidget.h"

namespace UE::UAI::DomainAdapters::CaptureMediaLongJob
{
	namespace
	{
		bool JsonTryGetString(const TSharedPtr<FJsonObject>& Object, const FString& Key, FString& OutValue)
		{
			return Object.IsValid() && Object->TryGetStringField(Key, OutValue);
		}
		bool JsonTryGetNumber(const TSharedPtr<FJsonObject>& Object, const FString& Key, double& OutValue)
		{
			return Object.IsValid() && Object->TryGetNumberField(Key, OutValue);
		}
		bool JsonTryGetBool(const TSharedPtr<FJsonObject>& Object, const FString& Key, bool& OutValue)
		{
			return Object.IsValid() && Object->TryGetBoolField(Key, OutValue);
		}
	}

	bool FBlueprintEditorCapture::Capture(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, UE::UAI::Contract::FDiagnostic& OutError)
	{
		using namespace CaptureDiagnostics;
		if (!OutData.IsValid())
		{
			OutData = MakeShared<FJsonObject>();
		}
	
		FString AssetPathInput;
		if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
		{
			OutError = Input(TEXT("missing_asset_path"), TEXT("Blueprint capture requires a non-empty asset_path."),
				TEXT("blueprint_capture"), TEXT("/asset_path"), Ctx.Params.IsValid() ? Ctx.Params->TryGetField(TEXT("asset_path")) : nullptr,
				MakeShared<FJsonValueString>(TEXT("non-empty Blueprint asset path")));
			return false;
		}
	
		ON_SCOPE_EXIT { if (!OutError.IsEmpty() && OutError.TargetPath.IsEmpty()) OutError.TargetPath = AssetPathInput; };
		UBlueprint* Blueprint = UeAgentBlueprintAuthoringOps::LoadBlueprintAsset(AssetPathInput);
		if (!Blueprint)
		{
			OutError = Input(TEXT("blueprint_not_found"), TEXT("The requested Blueprint asset could not be loaded."),
				TEXT("blueprint_capture"), TEXT("/asset_path"), MakeShared<FJsonValueString>(AssetPathInput), MakeShared<FJsonValueString>(TEXT("loadable Blueprint asset")));
			return false;
		}
	
		FString Target = TEXT("viewport");
		JsonTryGetString(Ctx.Params, TEXT("target"), Target);
		Target = Target.TrimStartAndEnd().ToLower();
		bool bAllowUiMutation = false;
		JsonTryGetBool(Ctx.Params, TEXT("allow_ui_mutation"), bAllowUiMutation);
		bool bAllowWindowMutation = false;
		JsonTryGetBool(Ctx.Params, TEXT("allow_window_mutation"), bAllowWindowMutation);
		if (bAllowUiMutation)
		{
			bAllowWindowMutation = true;
		}
	
		bool bOpenEditorIfNeeded = false;
		JsonTryGetBool(Ctx.Params, TEXT("open_editor_if_needed"), bOpenEditorIfNeeded);
	
		TSharedPtr<IBlueprintEditor> BlueprintEditor = UeAgentBlueprintEditorCapture::GetBlueprintEditor(Blueprint, bOpenEditorIfNeeded, OutError);
		if (!BlueprintEditor.IsValid())
		{
			return false;
		}
	
		FString GraphName = TEXT("EventGraph");
		JsonTryGetString(Ctx.Params, TEXT("graph_name"), GraphName);
		bool bRestoreGraphViewAfterCapture = true;
		JsonTryGetBool(Ctx.Params, TEXT("restore_graph_view_after_capture"), bRestoreGraphViewAfterCapture);
	
		FString Format = TEXT("jpg");
		int32 Quality = 85;
		int32 MaxSize = 0;
		JsonTryGetString(Ctx.Params, TEXT("format"), Format);
		double QualityNum = (double)Quality;
		if (JsonTryGetNumber(Ctx.Params, TEXT("quality"), QualityNum))
		{
			Quality = FMath::Clamp((int32)QualityNum, 1, 100);
		}
		double MaxSizeNum = (double)MaxSize;
		if (JsonTryGetNumber(Ctx.Params, TEXT("max_size"), MaxSizeNum))
		{
			MaxSize = FMath::Max((int32)MaxSizeNum, 1);
		}
		double CaptureWidthNum = 0.0;
		double CaptureHeightNum = 0.0;
		const bool bHasCaptureWidth = JsonTryGetNumber(Ctx.Params, TEXT("capture_width"), CaptureWidthNum);
		const bool bHasCaptureHeight = JsonTryGetNumber(Ctx.Params, TEXT("capture_height"), CaptureHeightNum);
		const bool bHasGraphCaptureDrawSize = bHasCaptureWidth || bHasCaptureHeight;
		FVector2D GraphCaptureDrawSize(1440.0, 900.0);
		if (bHasGraphCaptureDrawSize)
		{
			if (bHasCaptureWidth)
			{
				GraphCaptureDrawSize.X = CaptureWidthNum;
			}
			if (bHasCaptureHeight)
			{
				GraphCaptureDrawSize.Y = CaptureHeightNum;
			}
			GraphCaptureDrawSize.X = FMath::Max(FMath::RoundToDouble(GraphCaptureDrawSize.X), 1.0);
			GraphCaptureDrawSize.Y = FMath::Max(FMath::RoundToDouble(GraphCaptureDrawSize.Y), 1.0);
		}
		Format = Format.ToLower();
		if (Format == TEXT("jpeg"))
		{
			Format = TEXT("jpg");
		}
		if (Format != TEXT("png") && Format != TEXT("jpg") && Format != TEXT("webp"))
		{
			OutError = Input(TEXT("unsupported_format"), TEXT("The Blueprint capture image format is unsupported."),
				TEXT("blueprint_capture"), TEXT("/format"), Ctx.Params->TryGetField(TEXT("format")), MakeShared<FJsonValueString>(TEXT("png, jpg/jpeg or webp")));
			return false;
		}
	
		TSharedPtr<SWidget> WidgetToShot;
		TSharedPtr<SEditorViewport> ViewportWidgetForShot;
		TSharedPtr<SWidget> ViewportSceneWidgetForShot;
		TSharedPtr<SWindow> BlueprintWindowForShot;
		TSharedPtr<SGraphEditor> GraphEditorToRestore;
		TSharedPtr<SGraphEditor> GraphEditorForViewState;
		TSharedPtr<SGraphEditor> OwnedGraphEditorForShot;
		UEdGraph* GraphToRestore = nullptr;
		bool bRestoredGraphViewAfterCapture = false;
		bool bWindowMutatedForCapture = false;
		bool bPreparedWindowForCapture = false;
		bool bGraphCaptureUsedOwnedEditor = false;
		FVector2f GraphViewBeforeCapture(0.0f, 0.0f);
		float GraphZoomBeforeCapture = 1.0f;
		FVector2f GraphViewAfterCapture(0.0f, 0.0f);
		float GraphZoomAfterCapture = 1.0f;
		bool bCapturedGraphViewBefore = false;
		bool bCapturedGraphViewAfter = false;
		FString GraphViewRestoreMode = TEXT("none");
		FString GraphCaptureWidgetSource = TEXT("none");
		FString EffectiveTarget = Target;
		auto RestoreGraphViewAfterCapture = [&]() -> bool
		{
			if (!bRestoreGraphViewAfterCapture || !GraphToRestore || !GraphEditorToRestore.IsValid())
			{
				return false;
			}
			if (!bAllowUiMutation && bCapturedGraphViewBefore)
			{
				float RestoreZoomAmount = UeAgentGraphViewUtils::SafeGraphZoomAmount(GraphZoomBeforeCapture);
				FVector2f RestoreViewLocation = UeAgentGraphViewUtils::SnapViewLocationToPixelGrid(
					GraphViewBeforeCapture,
					RestoreZoomAmount);
				GraphEditorToRestore->SetViewLocation(RestoreViewLocation, RestoreZoomAmount);
				GraphEditorToRestore->GetViewLocation(RestoreViewLocation, RestoreZoomAmount);
				GraphViewRestoreMode = TEXT("pre_capture");
				FUeAgentInterfaceLogger::Log(
					TEXT("CmdBlueprintScreenshot restored graph view to pre-capture state asset=%s graph=%s view=(%.3f,%.3f) zoom=%.6f"),
					*Blueprint->GetOutermost()->GetName(),
					*GraphToRestore->GetName(),
					RestoreViewLocation.X,
					RestoreViewLocation.Y,
					RestoreZoomAmount);
				return true;
			}
			FVector2f RestoredViewLocation(0.0f, 0.0f);
			float RestoredZoomAmount = 1.0f;
			const bool bRestored = UeAgentBlueprintEditorCapture::RestoreBlueprintGraphEditorToDocumentViewState(
				Blueprint,
				GraphToRestore,
				GraphEditorToRestore,
				&RestoredViewLocation,
				&RestoredZoomAmount);
			if (bRestored)
			{
				FUeAgentInterfaceLogger::Log(
					TEXT("CmdBlueprintScreenshot restored graph view after capture asset=%s graph=%s view=(%.3f,%.3f) zoom=%.6f"),
					*Blueprint->GetOutermost()->GetName(),
					*GraphToRestore->GetName(),
					RestoredViewLocation.X,
					RestoredViewLocation.Y,
					RestoredZoomAmount);
				GraphViewRestoreMode = TEXT("document");
			}
			return bRestored;
		};
		auto ResolveGraphWidgetForShot = [&](UEdGraph* Graph) -> bool
		{
			if (!Graph)
			{
				OutError = Native(TEXT("graph_not_found"), TEXT("The Blueprint graph selected for capture is unavailable."),
					TEXT("blueprint_capture"), TEXT("graph_name"), MakeShared<FJsonValueString>(GraphName), MakeShared<FJsonValueString>(TEXT("existing Blueprint graph")));
				return false;
			}
	
			GraphToRestore = Graph;
			GraphName = Graph->GetName();
			EffectiveTarget = TEXT("graph");
			if (!bAllowUiMutation)
			{
				FVector2f ShotViewLocation(0.0f, 0.0f);
				float ShotZoomAmount = 1.0f;
				const TSharedPtr<SGraphEditor> LiveGraphEditor = SGraphEditor::FindGraphEditorForGraph(Graph);
				if (LiveGraphEditor.IsValid())
				{
					GraphEditorForViewState = LiveGraphEditor;
					LiveGraphEditor->GetViewLocation(GraphViewBeforeCapture, GraphZoomBeforeCapture);
					GraphZoomBeforeCapture = UeAgentGraphViewUtils::SafeGraphZoomAmount(GraphZoomBeforeCapture);
					ShotViewLocation = GraphViewBeforeCapture;
					ShotZoomAmount = GraphZoomBeforeCapture;
					bCapturedGraphViewBefore = true;
					GraphCaptureWidgetSource = TEXT("offscreen_snapshot_from_live_view");
				}
				else if (UeAgentBlueprintEditorCapture::TryGetBlueprintGraphDocumentViewState(Blueprint, Graph, ShotViewLocation, ShotZoomAmount))
				{
					GraphCaptureWidgetSource = TEXT("offscreen_snapshot_from_document_view");
				}
				else
				{
					GraphCaptureWidgetSource = TEXT("offscreen_snapshot_from_default_view");
				}
	
				OwnedGraphEditorForShot = UeAgentBlueprintEditorCapture::CreateOffscreenGraphEditorForCapture(
					Graph,
					TEXT("UAI graph screenshot"),
					OutError);
				if (!OwnedGraphEditorForShot.IsValid())
				{
					return false;
				}
				UeAgentBlueprintEditorCapture::PrepareOffscreenGraphEditorForCapture(
					OwnedGraphEditorForShot,
					ShotViewLocation,
					ShotZoomAmount);
				WidgetToShot = OwnedGraphEditorForShot;
				bGraphCaptureUsedOwnedEditor = true;
				GraphViewRestoreMode = TEXT("not_needed_offscreen_snapshot");
				return true;
			}
	
			TSharedPtr<SGraphEditor> GraphEditor = BlueprintEditor->OpenGraphAndBringToFront(Graph, true);
			if (!GraphEditor.IsValid())
			{
				OutError = Native(TEXT("graph_editor_not_found"), TEXT("The Blueprint editor could not open a graph editor for capture."),
					TEXT("blueprint_capture"), TEXT("graph_editor_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.TargetPath = Graph->GetPathName();
				return false;
			}
			GraphEditorToRestore = GraphEditor;
			GraphEditorForViewState = GraphEditor;
			GraphEditor->GetViewLocation(GraphViewBeforeCapture, GraphZoomBeforeCapture);
			GraphZoomBeforeCapture = UeAgentGraphViewUtils::SafeGraphZoomAmount(GraphZoomBeforeCapture);
			bCapturedGraphViewBefore = true;
			GraphCaptureWidgetSource = TEXT("live_graph_editor");
			UE::UAI::Contract::FDiagnostic WindowError;
			BlueprintWindowForShot = UeAgentBlueprintEditorCapture::ResolveBlueprintEditorWindow(BlueprintEditor, WindowError);
			if (BlueprintWindowForShot.IsValid() && bAllowWindowMutation)
			{
				bool bWindowMutatedThisPass = false;
				bPreparedWindowForCapture = true;
				if (!EditorCapture::SlateWindow::Prepare(BlueprintWindowForShot, OutError, bAllowWindowMutation, &bWindowMutatedThisPass))
				{
					bWindowMutatedForCapture = bWindowMutatedForCapture || bWindowMutatedThisPass;
					bRestoredGraphViewAfterCapture = RestoreGraphViewAfterCapture();
					return false;
				}
				bWindowMutatedForCapture = bWindowMutatedForCapture || bWindowMutatedThisPass;
			}
			WidgetToShot = GraphEditor;
			return true;
		};
		if (Target == TEXT("viewport") || Target == TEXT("components"))
		{
			ViewportWidgetForShot = UeAgentBlueprintEditorCapture::ResolveBlueprintViewportWidget(BlueprintEditor, OutError);
			if (!ViewportWidgetForShot.IsValid())
			{
				return false;
			}
			WidgetToShot = ViewportWidgetForShot;
			ViewportSceneWidgetForShot = UeAgentBlueprintEditorCapture::ResolveBlueprintViewportSceneWidget(ViewportWidgetForShot.ToSharedRef());
			EffectiveTarget = TEXT("viewport");
		}
		else if (Target == TEXT("eventgraph") || Target == TEXT("event_graph"))
		{
			UEdGraph* Graph = UeAgentBlueprintAuthoringOps::ResolveBlueprintGraph(Blueprint, TEXT("EventGraph"));
			if (!Graph)
			{
				OutError = Native(TEXT("graph_not_found"), TEXT("The Blueprint does not contain an EventGraph for capture."),
					TEXT("blueprint_capture"), TEXT("graph_name"), MakeShared<FJsonValueString>(TEXT("EventGraph")), MakeShared<FJsonValueString>(TEXT("existing Blueprint graph")));
				return false;
			}
			if (!ResolveGraphWidgetForShot(Graph))
			{
				return false;
			}
		}
		else if (Target == TEXT("graph") || Target.StartsWith(TEXT("graph:")))
		{
			if (Target.StartsWith(TEXT("graph:")))
			{
				GraphName = Target.Mid(6).TrimStartAndEnd();
			}
	
			UEdGraph* Graph = UeAgentBlueprintAuthoringOps::ResolveBlueprintGraph(Blueprint, GraphName);
			if (!Graph)
			{
				const FString Pointer = Target.StartsWith(TEXT("graph:")) ? TEXT("/target")
					: Ctx.Params->HasField(TEXT("graph_name")) ? TEXT("/graph_name") : FString();
				OutError = Input(TEXT("graph_not_found"), TEXT("The Blueprint does not contain the requested graph."),
					TEXT("blueprint_capture"), Pointer, MakeShared<FJsonValueString>(GraphName), MakeShared<FJsonValueString>(TEXT("existing Blueprint graph")));
				return false;
			}
			if (!ResolveGraphWidgetForShot(Graph))
			{
				return false;
			}
		}
		else if (Target == TEXT("window"))
		{
			BlueprintWindowForShot = UeAgentBlueprintEditorCapture::ResolveBlueprintEditorWindow(BlueprintEditor, OutError);
			if (!BlueprintWindowForShot.IsValid())
			{
				return false;
			}
			WidgetToShot = BlueprintWindowForShot;
			EffectiveTarget = TEXT("window");
			bool bWindowMutatedThisPass = false;
			bPreparedWindowForCapture = true;
				if (!EditorCapture::SlateWindow::Prepare(BlueprintWindowForShot, OutError, bAllowWindowMutation, &bWindowMutatedThisPass))
			{
				bWindowMutatedForCapture = bWindowMutatedForCapture || bWindowMutatedThisPass;
				return false;
			}
			bWindowMutatedForCapture = bWindowMutatedForCapture || bWindowMutatedThisPass;
		}
		else
		{
			OutError = Input(TEXT("invalid_target"), TEXT("The Blueprint capture target is unsupported."),
				TEXT("blueprint_capture"), TEXT("/target"), Ctx.Params->TryGetField(TEXT("target")),
				MakeShared<FJsonValueString>(TEXT("viewport, eventgraph/event_graph, graph, graph:<name> or window")));
			return false;
		}
	
		if (!WidgetToShot.IsValid())
		{
			OutError = Native(TEXT("target_widget_not_found"), TEXT("The Blueprint capture target has no widget to render."),
				TEXT("blueprint_capture"), TEXT("target_widget"), MakeShared<FJsonValueString>(EffectiveTarget), MakeShared<FJsonValueString>(TEXT("available target widget")));
			return false;
		}
	
		TArray<FColor> Pixels;
		FIntPoint ShotSize(0, 0);
		bool bShotOk = false;
		FString CaptureMode = TEXT("slate_widget_offscreen");
		if (EffectiveTarget == TEXT("viewport") && ViewportWidgetForShot.IsValid())
		{
		UeAgentBlueprintEditorCapture::PrepareBlueprintViewportForCapture(BlueprintEditor, Blueprint, ViewportWidgetForShot);
	
			if (!BlueprintWindowForShot.IsValid())
			{
				UE::UAI::Contract::FDiagnostic WindowError;
				BlueprintWindowForShot = UeAgentBlueprintEditorCapture::ResolveBlueprintEditorWindow(BlueprintEditor, WindowError);
				if (!BlueprintWindowForShot.IsValid() && !WindowError.IsEmpty())
				{
					FUeAgentInterfaceLogger::Log(TEXT("CmdBlueprintScreenshot viewport window resolve failed before capture: %s"), *WindowError.ToPublicDetail());
				}
			}
			if (BlueprintWindowForShot.IsValid())
			{
				UE::UAI::Contract::FDiagnostic WindowPrepareError;
				bool bWindowMutatedThisPass = false;
				bPreparedWindowForCapture = true;
				if (!EditorCapture::SlateWindow::Prepare(BlueprintWindowForShot, WindowPrepareError, bAllowWindowMutation, &bWindowMutatedThisPass))
				{
					FUeAgentInterfaceLogger::Log(TEXT("CmdBlueprintScreenshot viewport window not renderable before capture: %s"), *WindowPrepareError.ToPublicDetail());
				}
				bWindowMutatedForCapture = bWindowMutatedForCapture || bWindowMutatedThisPass;
			}
	
			const TSharedRef<SWidget> CropWidget = ViewportSceneWidgetForShot.IsValid() ? ViewportSceneWidgetForShot.ToSharedRef() : WidgetToShot.ToSharedRef();
			if (ViewportSceneWidgetForShot.IsValid())
			{
				const FVector2D SceneWidgetSize = ViewportSceneWidgetForShot->GetTickSpaceGeometry().GetAbsoluteSize();
				FUeAgentInterfaceLogger::Log(
					TEXT("CmdBlueprintScreenshot viewport crop_widget=%s scene_widget_abs_size=(%.1f,%.1f)"),
					*ViewportSceneWidgetForShot->GetTypeAsString(),
					SceneWidgetSize.X,
					SceneWidgetSize.Y);
			}
			else
			{
				FUeAgentInterfaceLogger::Log(TEXT("CmdBlueprintScreenshot viewport crop_widget=fallback_editor_viewport"));
			}
	
			bool bWindowMutatedThisPass = false;
			bShotOk = UeAgentBlueprintEditorCapture::ScreenshotBlueprintWindowCropWidget(
				BlueprintEditor,
				CropWidget,
				Pixels,
				ShotSize,
				OutError,
				bAllowWindowMutation,
				&bWindowMutatedThisPass);
			bWindowMutatedForCapture = bWindowMutatedForCapture || bWindowMutatedThisPass;
			if (!bShotOk)
			{
				const UE::UAI::Contract::FDiagnostic CropError = OutError;
				FUeAgentInterfaceLogger::Log(TEXT("CmdBlueprintScreenshot viewport window-crop failed: %s, fallback to read-pixels"), *OutError.ToPublicDetail());
				UE::UAI::Contract::FDiagnostic FallbackError;
				if (UeAgentBlueprintEditorCapture::ScreenshotEditorViewportPixels(ViewportWidgetForShot.ToSharedRef(), Pixels, ShotSize, FallbackError))
				{
					bShotOk = true;
					CaptureMode = TEXT("viewport_readpixels_fallback");
					OutError.Reset();
				}
				else
				{
					bWindowMutatedThisPass = false;
					if (UeAgentBlueprintEditorCapture::ScreenshotBlueprintWindow(
						BlueprintEditor,
						Pixels,
						ShotSize,
						FallbackError,
						bAllowWindowMutation,
						&bWindowMutatedThisPass))
					{
						bWindowMutatedForCapture = bWindowMutatedForCapture || bWindowMutatedThisPass;
						bShotOk = true;
						CaptureMode = TEXT("window_offscreen_fallback");
						OutError.Reset();
					}
					else
					{
						bWindowMutatedForCapture = bWindowMutatedForCapture || bWindowMutatedThisPass;
						OutError = CropError;
						OutError.Append(FallbackError);
					}
				}
			}
			else
			{
				CaptureMode = TEXT("viewport_window_crop_offscreen");
			}
		}
		else
		{
			if (EffectiveTarget == TEXT("graph") && !bAllowUiMutation)
			{
				CaptureMode = TEXT("graph_widget_offscreen_noninvasive");
				bShotOk = UeAgentBlueprintEditorCapture::ScreenshotSlateWidgetNonInvasive(WidgetToShot.ToSharedRef(), GraphCaptureDrawSize, Pixels, ShotSize, OutError);
			}
			else
			{
				CaptureMode = EffectiveTarget == TEXT("graph") ? TEXT("graph_widget_offscreen_mutating") : TEXT("slate_widget_offscreen");
				bShotOk = UeAgentBlueprintEditorCapture::ScreenshotSlateWidget(WidgetToShot.ToSharedRef(), Pixels, ShotSize, OutError);
			}
		}
		if (EffectiveTarget == TEXT("graph"))
		{
			bRestoredGraphViewAfterCapture = RestoreGraphViewAfterCapture();
			if (GraphEditorForViewState.IsValid())
			{
				GraphEditorForViewState->GetViewLocation(GraphViewAfterCapture, GraphZoomAfterCapture);
				GraphZoomAfterCapture = UeAgentGraphViewUtils::SafeGraphZoomAmount(GraphZoomAfterCapture);
				bCapturedGraphViewAfter = true;
			}
		}
	
		if (!bShotOk)
		{
			FUeAgentInterfaceLogger::Log(TEXT("CmdBlueprintScreenshot failed target=%s asset=%s error=%s"), *EffectiveTarget, *Blueprint->GetOutermost()->GetName(), *OutError.ToPublicDetail());
			return false;
		}
	
		TArray<FColor> ResizedPixels;
		FIntPoint ResizedSize(0, 0);
		if (!UE::UAI::DomainAdapters::CaptureMediaLongJob::FCaptureImageProcessing::ResizePixelsMaxSize(Pixels, ShotSize, MaxSize, ResizedPixels, ResizedSize, OutError))
		{
			return false;
		}
	
		const FString OutPath = FViewportCapture::MakeShotPath(Format);
		if (!UE::UAI::DomainAdapters::CaptureMediaLongJob::FCaptureImageProcessing::WriteCompressedImage(ResizedPixels, ResizedSize, Format, Quality, OutPath, OutError))
		{
			return false;
		}
	
		const int64 Bytes = IFileManager::Get().FileSize(*OutPath);
		OutData->SetStringField(TEXT("asset_path"), Blueprint->GetOutermost()->GetName());
		OutData->SetStringField(TEXT("file_path"), OutPath);
		OutData->SetStringField(TEXT("target"), EffectiveTarget);
		OutData->SetStringField(TEXT("graph_name"), EffectiveTarget == TEXT("graph") ? GraphName : TEXT(""));
		OutData->SetStringField(TEXT("capture_mode"), CaptureMode);
		OutData->SetStringField(TEXT("legacy_backbuffer_capture"), TEXT("disabled"));
		OutData->SetBoolField(TEXT("allow_ui_mutation"), bAllowUiMutation);
		OutData->SetBoolField(TEXT("allow_window_mutation"), bAllowWindowMutation);
		OutData->SetBoolField(TEXT("prepared_window_for_capture"), bPreparedWindowForCapture);
		OutData->SetBoolField(TEXT("window_mutated_for_capture"), bWindowMutatedForCapture);
		OutData->SetBoolField(TEXT("capture_window_resolved"), BlueprintWindowForShot.IsValid());
		if (BlueprintWindowForShot.IsValid())
		{
			OutData->SetBoolField(TEXT("capture_window_visible_after"), BlueprintWindowForShot->IsVisible());
			OutData->SetBoolField(TEXT("capture_window_minimized_after"), BlueprintWindowForShot->IsWindowMinimized());
		}
		OutData->SetBoolField(TEXT("restore_graph_view_after_capture"), bRestoreGraphViewAfterCapture);
		OutData->SetBoolField(TEXT("restored_graph_view_after_capture"), bRestoredGraphViewAfterCapture);
		OutData->SetStringField(TEXT("graph_view_restore_mode"), GraphViewRestoreMode);
		if (EffectiveTarget == TEXT("graph"))
		{
			OutData->SetBoolField(TEXT("graph_capture_used_owned_editor"), bGraphCaptureUsedOwnedEditor);
			OutData->SetStringField(TEXT("graph_capture_widget_source"), GraphCaptureWidgetSource);
			OutData->SetBoolField(TEXT("captured_graph_view_before"), bCapturedGraphViewBefore);
			OutData->SetBoolField(TEXT("captured_graph_view_after"), bCapturedGraphViewAfter);
			OutData->SetNumberField(TEXT("graph_view_before_x"), GraphViewBeforeCapture.X);
			OutData->SetNumberField(TEXT("graph_view_before_y"), GraphViewBeforeCapture.Y);
			OutData->SetNumberField(TEXT("graph_zoom_before"), GraphZoomBeforeCapture);
			OutData->SetNumberField(TEXT("graph_view_after_x"), GraphViewAfterCapture.X);
			OutData->SetNumberField(TEXT("graph_view_after_y"), GraphViewAfterCapture.Y);
			OutData->SetNumberField(TEXT("graph_zoom_after"), GraphZoomAfterCapture);
			const bool bGraphViewChangedAfterCapture = bCapturedGraphViewBefore
				&& bCapturedGraphViewAfter
				&& (!FMath::IsNearlyEqual(GraphViewBeforeCapture.X, GraphViewAfterCapture.X, 0.01f)
					|| !FMath::IsNearlyEqual(GraphViewBeforeCapture.Y, GraphViewAfterCapture.Y, 0.01f)
					|| !FMath::IsNearlyEqual(GraphZoomBeforeCapture, GraphZoomAfterCapture, 0.0001f));
			OutData->SetBoolField(TEXT("graph_view_changed_after_capture"), bGraphViewChangedAfterCapture);
			const FVector2f GraphViewBeforeResidual = UeAgentGraphViewUtils::ViewLocationPixelResidual(
				GraphViewBeforeCapture,
				GraphZoomBeforeCapture);
			const FVector2f GraphViewAfterResidual = UeAgentGraphViewUtils::ViewLocationPixelResidual(
				GraphViewAfterCapture,
				GraphZoomAfterCapture);
			OutData->SetNumberField(TEXT("graph_view_before_pixel_residual_x"), GraphViewBeforeResidual.X);
			OutData->SetNumberField(TEXT("graph_view_before_pixel_residual_y"), GraphViewBeforeResidual.Y);
			OutData->SetNumberField(TEXT("graph_view_after_pixel_residual_x"), GraphViewAfterResidual.X);
			OutData->SetNumberField(TEXT("graph_view_after_pixel_residual_y"), GraphViewAfterResidual.Y);
		}
		OutData->SetStringField(TEXT("format"), Format);
		if (EffectiveTarget == TEXT("graph") && !bAllowUiMutation)
		{
			OutData->SetNumberField(TEXT("graph_capture_draw_width"), GraphCaptureDrawSize.X);
			OutData->SetNumberField(TEXT("graph_capture_draw_height"), GraphCaptureDrawSize.Y);
			OutData->SetBoolField(TEXT("graph_capture_draw_size_requested"), bHasGraphCaptureDrawSize);
		}
		OutData->SetNumberField(TEXT("width"), ResizedSize.X);
		OutData->SetNumberField(TEXT("height"), ResizedSize.Y);
		OutData->SetNumberField(TEXT("bytes"), (double)Bytes);
		return true;
	}

	bool FBlueprintEditorCapture::CaptureControlRig(
		const FUeAgentRequestContext& Context,
		TSharedPtr<FJsonObject>& OutData,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		FUeAgentRequestContext Proxy = Context;
		Proxy.Params = MakeShared<FJsonObject>();
		if (Context.Params.IsValid())
		{
			Proxy.Params->Values = Context.Params->Values;
		}
		FString GraphNameOrPath;
		if (!Proxy.Params->HasField(TEXT("graph_name"))
			&& Proxy.Params->TryGetStringField(TEXT("graph_name_or_path"), GraphNameOrPath)
			&& !GraphNameOrPath.TrimStartAndEnd().IsEmpty())
		{
			Proxy.Params->SetStringField(TEXT("graph_name"), GraphNameOrPath);
		}
		if (!Capture(Proxy, OutData, OutError))
		{
			return false;
		}
		OutData->SetStringField(TEXT("asset_kind"), TEXT("control_rig"));
		return true;
	}
}
