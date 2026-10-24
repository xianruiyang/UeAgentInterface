#include "Core/EditorCapture/Blueprint/UeAgentBlueprintEditorCaptureMechanics.h"
#include "Core/EditorCapture/UeAgentEditorWidgetTree.h"
#include "Core/EditorCapture/UeAgentSlateWidgetCapture.h"
#include "Core/EditorCapture/UeAgentSlateWindowCapture.h"
#include "Core/GraphLayout/UeAgentGraphViewUtils.h"
#include "BlueprintEditor.h"
#include "BlueprintEditorTabs.h"
#include "EditorViewportClient.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "GraphEditor.h"
#include "SGraphPanel.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Module/UeAgentInterfaceLogger.h"
#include "SEditorViewport.h"
#include "Slate/SceneViewport.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SWindow.h"
#include "Widgets/SWidget.h"
#include "UeAgentInterfaceSettings.h"
#include "UObject/GarbageCollection.h"

namespace UeAgentBlueprintEditorCapture
{
	static UE::UAI::Contract::FDiagnostic CaptureFailure(const TCHAR* Code, const FString& Reason, const FString& Name = FString())
	{
		auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("editor_capture"));
		Error.CandidateName = Name; Error.SourceKind = TEXT("native_result"); return Error;
	}
	bool PersistBlueprintGraphDocumentViewState(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const FVector2f& ViewLocation,
		float ZoomAmount)
	{
		return PersistBlueprintGraphDocumentViewState(Blueprint, Graph, ViewLocation, ZoomAmount, nullptr);
	}

	bool PersistBlueprintGraphDocumentViewState(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const FVector2f& ViewLocation,
		float ZoomAmount,
		int32* OutRemovedExistingEntryCount)
	{
		if (OutRemovedExistingEntryCount)
		{
			*OutRemovedExistingEntryCount = 0;
		}
		if (!Blueprint || !Graph)
		{
			return false;
		}

		const FSoftObjectPath GraphPath(Graph);
		const int32 BeforeCount = Blueprint->LastEditedDocuments.Num();
		Blueprint->LastEditedDocuments.RemoveAll(
			[&GraphPath](const FEditedDocumentInfo& Info)
			{
				return Info.EditedObjectPath == GraphPath;
			});
		if (OutRemovedExistingEntryCount)
		{
			*OutRemovedExistingEntryCount = BeforeCount - Blueprint->LastEditedDocuments.Num();
		}

		const FVector2f SnappedViewLocation = UeAgentGraphViewUtils::SnapViewLocationToPixelGrid(ViewLocation, ZoomAmount);
		Blueprint->LastEditedDocuments.Add(FEditedDocumentInfo(Graph, SnappedViewLocation, ZoomAmount));
		return true;
	}

	bool TryGetBlueprintGraphDocumentViewState(
		const UBlueprint* Blueprint,
		const UEdGraph* Graph,
		FVector2f& OutViewLocation,
		float& OutZoomAmount)
	{
		if (!Blueprint || !Graph)
		{
			return false;
		}

		const FSoftObjectPath GraphPath(Graph);
		for (int32 Index = Blueprint->LastEditedDocuments.Num() - 1; Index >= 0; --Index)
		{
			const FEditedDocumentInfo& Info = Blueprint->LastEditedDocuments[Index];
			if (Info.EditedObjectPath == GraphPath)
			{
				OutViewLocation = UeAgentGraphViewUtils::SnapViewLocationToPixelGrid(Info.SavedViewOffset, Info.SavedZoomAmount);
				OutZoomAmount = UeAgentGraphViewUtils::SafeGraphZoomAmount(Info.SavedZoomAmount);
				return true;
			}
		}
		return false;
	}

	bool RestoreBlueprintGraphEditorToDocumentViewState(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const TSharedPtr<SGraphEditor>& GraphEditor,
		FVector2f* OutRestoredViewLocation,
		float* OutRestoredZoomAmount)
	{
		if (!Blueprint || !Graph || !GraphEditor.IsValid())
		{
			return false;
		}

		FVector2f RestoreViewLocation(0.0f, 0.0f);
		float RestoreZoomAmount = 1.0f;
		if (!TryGetBlueprintGraphDocumentViewState(Blueprint, Graph, RestoreViewLocation, RestoreZoomAmount))
		{
			GraphEditor->GetViewLocation(RestoreViewLocation, RestoreZoomAmount);
			RestoreZoomAmount = UeAgentGraphViewUtils::SafeGraphZoomAmount(RestoreZoomAmount);
			RestoreViewLocation = UeAgentGraphViewUtils::SnapViewLocationToPixelGrid(RestoreViewLocation, RestoreZoomAmount);
		}

		GraphEditor->SetViewLocation(RestoreViewLocation, RestoreZoomAmount);
		GraphEditor->GetViewLocation(RestoreViewLocation, RestoreZoomAmount);
		RestoreZoomAmount = UeAgentGraphViewUtils::SafeGraphZoomAmount(RestoreZoomAmount);
		if (!UeAgentGraphViewUtils::IsViewLocationPixelAligned(RestoreViewLocation, RestoreZoomAmount))
		{
			RestoreViewLocation = UeAgentGraphViewUtils::SnapViewLocationToPixelGrid(RestoreViewLocation, RestoreZoomAmount);
			GraphEditor->SetViewLocation(RestoreViewLocation, RestoreZoomAmount);
			GraphEditor->GetViewLocation(RestoreViewLocation, RestoreZoomAmount);
		}
		if (OutRestoredViewLocation)
		{
			*OutRestoredViewLocation = RestoreViewLocation;
		}
		if (OutRestoredZoomAmount)
		{
			*OutRestoredZoomAmount = RestoreZoomAmount;
		}
		return true;
	}

	TSharedPtr<IBlueprintEditor> GetBlueprintEditor(UBlueprint* Blueprint, bool bOpenIfNeeded, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!Blueprint)
		{
			OutError = CaptureFailure(TEXT("blueprint_not_found"), TEXT("The Blueprint required for editor capture is unavailable."));
			return nullptr;
		}

		TSharedPtr<IBlueprintEditor> BlueprintEditor = FKismetEditorUtilities::GetIBlueprintEditorForObject(Blueprint, bOpenIfNeeded);
		if (!BlueprintEditor.IsValid())
		{
			OutError = CaptureFailure(bOpenIfNeeded ? TEXT("blueprint_editor_open_failed") : TEXT("blueprint_editor_not_opened"), bOpenIfNeeded ? TEXT("Unreal could not open the Blueprint editor required for capture.") : TEXT("The Blueprint editor is not open and this request does not open it."), Blueprint->GetName()); OutError.TargetPath = Blueprint->GetPathName();
			return nullptr;
		}
		return BlueprintEditor;
	}

	TSharedPtr<SGraphEditor> CreateOffscreenGraphEditorForCapture(UEdGraph* Graph, const FString& CornerText, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!Graph)
		{
			OutError = CaptureFailure(TEXT("graph_not_found"), TEXT("The graph required to create an offscreen graph editor is unavailable."));
			return nullptr;
		}

		FGraphAppearanceInfo AppearanceInfo;
		AppearanceInfo.CornerText = FText::FromString(CornerText);
		TSharedPtr<SGraphEditor> GraphEditor = SNew(SGraphEditor)
			.IsEditable(false)
			.DisplayAsReadOnly(true)
			.ShowGraphStateOverlay(false)
			.Appearance(AppearanceInfo)
			.GraphToEdit(Graph);
		if (!GraphEditor.IsValid())
		{
			OutError = CaptureFailure(TEXT("offscreen_graph_editor_create_failed"), TEXT("Slate could not create the read-only offscreen graph editor."), Graph->GetName());
			return nullptr;
		}
		return GraphEditor;
	}

	static TSharedPtr<SDockTab> ResolveBlueprintTab(const TSharedPtr<IBlueprintEditor>& BlueprintEditor, const FName& TabId, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!BlueprintEditor.IsValid())
		{
			OutError = CaptureFailure(TEXT("blueprint_editor_not_opened"), TEXT("The Blueprint editor is unavailable, so the requested capture tab cannot be resolved."), TabId.ToString());
			return nullptr;
		}

		const TSharedPtr<FTabManager> TabManager = BlueprintEditor->GetTabManager();
		if (!TabManager.IsValid())
		{
			OutError = CaptureFailure(TEXT("blueprint_tab_manager_not_found"), TEXT("The Blueprint editor has no tab manager to resolve the requested capture tab."), TabId.ToString());
			return nullptr;
		}

		TSharedPtr<SDockTab> Tab = TabManager->FindExistingLiveTab(FTabId(TabId));
		if (!Tab.IsValid())
		{
			Tab = TabManager->TryInvokeTab(FTabId(TabId), false);
		}
		if (!Tab.IsValid())
		{
			OutError = CaptureFailure(TEXT("blueprint_tab_not_found"), TEXT("The Blueprint capture tab could neither be found nor opened."), TabId.ToString());
			return nullptr;
		}
		Tab->ActivateInParent(ETabActivationCause::SetDirectly);
		return Tab;
	}

	TSharedPtr<SWindow> ResolveBlueprintEditorWindow(const TSharedPtr<IBlueprintEditor>& BlueprintEditor, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!BlueprintEditor.IsValid())
		{
			OutError = CaptureFailure(TEXT("blueprint_editor_not_opened"), TEXT("The Blueprint editor is unavailable, so its capture window cannot be resolved."));
			return nullptr;
		}

		const TSharedPtr<FTabManager> TabManager = BlueprintEditor->GetTabManager();
		if (!TabManager.IsValid())
		{
			OutError = CaptureFailure(TEXT("blueprint_tab_manager_not_found"), TEXT("The Blueprint editor has no tab manager from which to resolve its capture window."));
			return nullptr;
		}

		const TSharedPtr<SDockTab> OwnerTab = TabManager->GetOwnerTab();
		if (!OwnerTab.IsValid())
		{
			OutError = CaptureFailure(TEXT("blueprint_owner_tab_not_found"), TEXT("The Blueprint tab manager has no owner tab for window capture."));
			return nullptr;
		}

		const TSharedPtr<SWindow> ParentWindow = OwnerTab->GetParentWindow();
		if (!ParentWindow.IsValid())
		{
			OutError = CaptureFailure(TEXT("blueprint_window_not_found"), TEXT("The Blueprint editor's owner tab has no parent window."));
			return nullptr;
		}
		return ParentWindow;
	}

	TSharedPtr<SEditorViewport> ResolveBlueprintViewportWidget(const TSharedPtr<IBlueprintEditor>& BlueprintEditor, UE::UAI::Contract::FDiagnostic& OutError)
	{
		const TArray<FName> CandidateTabIds = {
			FBlueprintEditorTabs::SCSViewportID,
			FName(TEXT("Viewport")),
			FName(TEXT("PreviewViewport")),
			FName(TEXT("AnimBlueprintPreviewEditor"))
		};

		auto ResolveViewportFromTab = [](const TSharedPtr<SDockTab>& ViewportTab) -> TSharedPtr<SEditorViewport>
		{
			if (!ViewportTab.IsValid())
			{
				return nullptr;
			}

			const TSharedPtr<SWidget> RootWidget = ViewportTab->GetContent();
			if (!RootWidget.IsValid())
			{
				return nullptr;
			}

			TSharedPtr<SWidget> FoundWidget;
			if (!UE::UAI::EditorCapture::WidgetTree::FindFirstByType(RootWidget.ToSharedRef(), FName(TEXT("SSCSEditorViewport")), FoundWidget) &&
				!UE::UAI::EditorCapture::WidgetTree::FindFirstByType(RootWidget.ToSharedRef(), FName(TEXT("SAnimationEditorViewport")), FoundWidget) &&
				!UE::UAI::EditorCapture::WidgetTree::FindFirstByType(RootWidget.ToSharedRef(), FName(TEXT("SEditorViewport")), FoundWidget))
			{
				return nullptr;
			}

			const TSharedPtr<SEditorViewport> ViewportWidget = StaticCastSharedPtr<SEditorViewport>(FoundWidget);
			if (ViewportWidget.IsValid() && ViewportWidget->GetViewportClient().IsValid())
			{
				return ViewportWidget;
			}
			return nullptr;
		};

		const TSharedPtr<FTabManager> TabManager = BlueprintEditor.IsValid() ? BlueprintEditor->GetTabManager() : nullptr;
		if (TabManager.IsValid())
		{
			for (const FName& TabId : CandidateTabIds)
			{
				const TSharedPtr<SDockTab> ViewportTab = TabManager->FindExistingLiveTab(FTabId(TabId));
				const TSharedPtr<SEditorViewport> ViewportWidget = ResolveViewportFromTab(ViewportTab);
				if (ViewportWidget.IsValid())
				{
					return ViewportWidget;
				}
			}
		}

		for (const FName& TabId : CandidateTabIds)
		{
			UE::UAI::Contract::FDiagnostic LocalError;
			const TSharedPtr<SDockTab> ViewportTab = ResolveBlueprintTab(BlueprintEditor, TabId, LocalError);
			const TSharedPtr<SEditorViewport> ViewportWidget = ResolveViewportFromTab(ViewportTab);
			if (ViewportWidget.IsValid())
			{
				return ViewportWidget;
			}
		}

		OutError = CaptureFailure(TEXT("blueprint_viewport_widget_not_found"), TEXT("No editor viewport widget was found in the Blueprint editor's available viewport tabs."));
		TArray<TSharedPtr<FJsonValue>> TriedTabs; for (const FName& TabId : CandidateTabIds) TriedTabs.Add(MakeShared<FJsonValueString>(TabId.ToString())); OutError.ActualValue = MakeShared<FJsonValueArray>(MoveTemp(TriedTabs));
		return nullptr;
	}

	TSharedPtr<SWidget> ResolveBlueprintViewportSceneWidget(const TSharedRef<SEditorViewport>& ViewportWidget)
	{
		TArray<TSharedRef<SWidget>> ViewportChildren;
		UE::UAI::EditorCapture::WidgetTree::FindAllByType(ViewportWidget, FName(TEXT("SViewport")), ViewportChildren);
		if (ViewportChildren.Num() <= 0)
		{
			return nullptr;
		}

		TSharedRef<SWidget> BestWidget = ViewportChildren[0];
		float BestArea = 0.0f;
		for (const TSharedRef<SWidget>& Candidate : ViewportChildren)
		{
			const FVector2D Size = Candidate->GetTickSpaceGeometry().GetAbsoluteSize();
			const float Area = Size.X * Size.Y;
			if (Area > BestArea)
			{
				BestArea = Area;
				BestWidget = Candidate;
			}
		}
		return BestWidget;
	}

	void PrepareBlueprintViewportForCapture(const TSharedPtr<IBlueprintEditor>& BlueprintEditor, UBlueprint* Blueprint, const TSharedPtr<SEditorViewport>& ViewportWidget)
	{
		if (BlueprintEditor.IsValid())
		{
			const TSharedPtr<FBlueprintEditor> ConcreteBlueprintEditor = StaticCastSharedPtr<FBlueprintEditor>(BlueprintEditor);
			if (ConcreteBlueprintEditor.IsValid() && Blueprint)
			{
				ConcreteBlueprintEditor->UpdatePreviewActor(Blueprint, true);

				AActor* PreviewActor = ConcreteBlueprintEditor->GetPreviewActor();
				if (PreviewActor)
				{
					const FBox Bounds = PreviewActor->GetComponentsBoundingBox(true);
					FUeAgentInterfaceLogger::Log(
						TEXT("PrepareBlueprintViewportForCapture preview_actor=%s bounds_min=(%.1f,%.1f,%.1f) bounds_max=(%.1f,%.1f,%.1f)"),
						*PreviewActor->GetName(),
						Bounds.Min.X,
						Bounds.Min.Y,
						Bounds.Min.Z,
						Bounds.Max.X,
						Bounds.Max.Y,
						Bounds.Max.Z);
				}
				else
				{
					FUeAgentInterfaceLogger::Log(TEXT("PrepareBlueprintViewportForCapture preview_actor=null"));
				}
			}
		}

		if (!ViewportWidget.IsValid())
		{
			return;
		}

		if (const TSharedPtr<FEditorViewportClient> ViewportClient = ViewportWidget->GetViewportClient())
		{
			const bool bNeedsInitialFocus =
				ViewportClient->GetViewLocation().IsNearlyZero(0.01f) &&
				ViewportClient->GetViewRotation().Equals(FRotator::ZeroRotator, 0.01f);
			if (bNeedsInitialFocus)
			{
				const TSharedPtr<FBlueprintEditor> ConcreteBlueprintEditor = BlueprintEditor.IsValid() ? StaticCastSharedPtr<FBlueprintEditor>(BlueprintEditor) : nullptr;
				if (ConcreteBlueprintEditor.IsValid() && ConcreteBlueprintEditor->GetPreviewActor())
				{
					const FBox PreviewBounds = ConcreteBlueprintEditor->GetPreviewActor()->GetComponentsBoundingBox(true);
					FUeAgentInterfaceLogger::Log(
						TEXT("PrepareBlueprintViewportForCapture viewport camera looks uninitialized -> FocusViewportOnBox bounds_min=(%.1f,%.1f,%.1f) bounds_max=(%.1f,%.1f,%.1f)"),
						PreviewBounds.Min.X,
						PreviewBounds.Min.Y,
						PreviewBounds.Min.Z,
						PreviewBounds.Max.X,
						PreviewBounds.Max.Y,
						PreviewBounds.Max.Z);
					ViewportClient->FocusViewportOnBox(PreviewBounds, true);
				}
			}

			ViewportClient->Invalidate();
			FUeAgentInterfaceLogger::Log(
				TEXT("PrepareBlueprintViewportForCapture viewport_client realtime=%s view_loc=(%.1f,%.1f,%.1f) view_rot=(%.1f,%.1f,%.1f)"),
				ViewportClient->IsRealtime() ? TEXT("true") : TEXT("false"),
				ViewportClient->GetViewLocation().X,
				ViewportClient->GetViewLocation().Y,
				ViewportClient->GetViewLocation().Z,
				ViewportClient->GetViewRotation().Pitch,
				ViewportClient->GetViewRotation().Yaw,
				ViewportClient->GetViewRotation().Roll);
		}

		if (const TSharedPtr<FSceneViewport> SceneViewport = ViewportWidget->GetSceneViewport())
		{
			SceneViewport->InvalidateDisplay();
			SceneViewport->DeferInvalidateHitProxy();
			FUeAgentInterfaceLogger::Log(
				TEXT("PrepareBlueprintViewportForCapture scene_viewport size=(%d,%d)"),
				SceneViewport->GetSizeXY().X,
				SceneViewport->GetSizeXY().Y);
		}
	}

	static bool TryResolveOffscreenWidgetDrawSize(
		const TSharedRef<SWidget>& Widget,
		const bool bAllowFallbackSize,
		FVector2D& OutDrawSize,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		FVector2D DrawSize = Widget->GetTickSpaceGeometry().GetLocalSize();
		if (DrawSize.X <= 1.0 || DrawSize.Y <= 1.0)
		{
			DrawSize = Widget->GetTickSpaceGeometry().GetAbsoluteSize();
		}
		if (DrawSize.X <= 1.0 || DrawSize.Y <= 1.0)
		{
			DrawSize = Widget->GetCachedGeometry().GetLocalSize();
		}
		if (DrawSize.X <= 1.0 || DrawSize.Y <= 1.0)
		{
			DrawSize = Widget->GetDesiredSize();
		}
		if (DrawSize.X <= 1.0 || DrawSize.Y <= 1.0)
		{
			if (!bAllowFallbackSize)
			{
				OutError = CaptureFailure(TEXT("widget_geometry_unavailable_for_noninvasive_screenshot"), TEXT("The widget has no usable laid-out or desired capture size, and fallback dimensions are disabled."), Widget->GetTypeAsString()); OutError.PropertyPath = TEXT("draw_size"); OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("width and height > 1")); OutError.ActualValue = MakeShared<FJsonValueString>(DrawSize.ToString());
				return false;
			}
			DrawSize = FVector2D(1440.0, 900.0);
		}

		DrawSize.X = FMath::Max(FMath::RoundToDouble(DrawSize.X), 1.0);
		DrawSize.Y = FMath::Max(FMath::RoundToDouble(DrawSize.Y), 1.0);
		OutDrawSize = DrawSize;
		return true;
	}

	static FVector2D ResolveOffscreenWidgetDrawSize(const TSharedRef<SWidget>& Widget)
	{
		FVector2D DrawSize(1440.0, 900.0);
		UE::UAI::Contract::FDiagnostic Error;
		TryResolveOffscreenWidgetDrawSize(Widget, true, DrawSize, Error);
		return DrawSize;
	}


	bool ScreenshotSlateWidget(const TSharedRef<SWidget>& Widget, TArray<FColor>& OutPixels, FIntPoint& OutSize, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!FSlateApplication::IsInitialized())
		{
			OutError = CaptureFailure(TEXT("slate_not_initialized"), TEXT("Slate is not initialized, so the widget cannot be captured."), Widget->GetTypeAsString());
			return false;
		}

		FSlateApplication::Get().PumpMessages();
		const float WidgetScale = FSlateApplication::Get().GetApplicationScale();
		Widget->SlatePrepass(WidgetScale);

		const FVector2D DrawSize = ResolveOffscreenWidgetDrawSize(Widget);
		return UeAgentSlateWidgetCapture::CaptureWidgetPixels(Widget, DrawSize, 1.0f, OutPixels, OutSize, OutError);
	}

	bool ScreenshotSlateWidgetNonInvasive(const TSharedRef<SWidget>& Widget, TArray<FColor>& OutPixels, FIntPoint& OutSize, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!FSlateApplication::IsInitialized())
		{
			OutError = CaptureFailure(TEXT("slate_not_initialized"), TEXT("Slate is not initialized, so the widget cannot be captured."), Widget->GetTypeAsString());
			return false;
		}

		FVector2D DrawSize(0.0, 0.0);
		if (!TryResolveOffscreenWidgetDrawSize(Widget, false, DrawSize, OutError))
		{
			OutError.Reset();
			if (!TryResolveOffscreenWidgetDrawSize(Widget, true, DrawSize, OutError))
			{
				return false;
			}
			FUeAgentInterfaceLogger::Log(
				TEXT("ScreenshotSlateWidgetNonInvasive using fallback draw size for widget=%s size=(%.0f,%.0f)"),
				*Widget->GetTypeAsString(),
				DrawSize.X,
				DrawSize.Y);
		}
		return UeAgentSlateWidgetCapture::CaptureWidgetPixels(Widget, DrawSize, 1.0f, OutPixels, OutSize, OutError);
	}

	bool ScreenshotSlateWidgetNonInvasive(
		const TSharedRef<SWidget>& Widget,
		const FVector2D& RequestedDrawSize,
		TArray<FColor>& OutPixels,
		FIntPoint& OutSize,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!FSlateApplication::IsInitialized())
		{
			OutError = CaptureFailure(TEXT("slate_not_initialized"), TEXT("Slate is not initialized, so the widget cannot be captured."), Widget->GetTypeAsString());
			return false;
		}

		FVector2D DrawSize(
			FMath::Max(FMath::RoundToDouble(RequestedDrawSize.X), 1.0),
			FMath::Max(FMath::RoundToDouble(RequestedDrawSize.Y), 1.0));
		return UeAgentSlateWidgetCapture::CaptureWidgetPixels(Widget, DrawSize, 1.0f, OutPixels, OutSize, OutError);
	}

	void PrepareOffscreenGraphEditorForCapture(
		const TSharedPtr<SGraphEditor>& GraphEditor,
		const FVector2f& ViewLocation,
		const float ZoomAmount)
	{
		PrepareOffscreenGraphEditorForCapture(GraphEditor, ViewLocation, ZoomAmount, 2);
	}

	void PrepareOffscreenGraphEditorForCapture(
		const TSharedPtr<SGraphEditor>& GraphEditor,
		const FVector2f& ViewLocation,
		const float ZoomAmount,
		const int32 FrameCount)
	{
		if (!GraphEditor.IsValid() || !FSlateApplication::IsInitialized())
		{
			return;
		}

		const float SafeZoom = UeAgentGraphViewUtils::SafeGraphZoomAmount(ZoomAmount);
		const FVector2f SnappedViewLocation = UeAgentGraphViewUtils::SnapViewLocationToPixelGrid(ViewLocation, SafeZoom);
		GraphEditor->SetViewLocation(SnappedViewLocation, SafeZoom);
		SGraphPanel* GraphPanel = GraphEditor->GetGraphPanel();
		for (int32 FrameIndex = 0; FrameIndex < FMath::Max(1, FrameCount); ++FrameIndex)
		{
			if (GraphPanel)
			{
				GraphPanel->Update();
			}
			UE::UAI::EditorCapture::SlateWindow::PumpAndTick();
			const float WidgetScale = FSlateApplication::Get().GetApplicationScale();
			GraphEditor->SlatePrepass(WidgetScale);
			if (GraphPanel)
			{
				GraphPanel->SlatePrepass(WidgetScale);
			}
		}
	}

	bool ScreenshotEditorViewportPixels(const TSharedRef<SEditorViewport>& ViewportWidget, TArray<FColor>& OutPixels, FIntPoint& OutSize, UE::UAI::Contract::FDiagnostic& OutError)
	{
		const TSharedPtr<FEditorViewportClient> ViewportClient = ViewportWidget->GetViewportClient();
		if (!ViewportClient.IsValid())
		{
			OutError = CaptureFailure(TEXT("blueprint_viewport_client_not_found"), TEXT("The editor viewport widget has no viewport client."), ViewportWidget->GetTypeAsString());
			return false;
		}

		FViewport* RawViewport = ViewportClient->Viewport;
		if (!RawViewport)
		{
			OutError = CaptureFailure(TEXT("blueprint_viewport_not_found"), TEXT("The editor viewport client has no native viewport from which to read pixels."), ViewportWidget->GetTypeAsString());
			const FGeometry& WidgetGeometry = ViewportWidget->GetTickSpaceGeometry();
			FUeAgentInterfaceLogger::Log(
				TEXT("ScreenshotEditorViewportPixels missing raw viewport widget_local=(%.1f,%.1f) widget_abs=(%.1f,%.1f) widget_pos=(%.1f,%.1f)"),
				WidgetGeometry.GetLocalSize().X,
				WidgetGeometry.GetLocalSize().Y,
				WidgetGeometry.GetAbsoluteSize().X,
				WidgetGeometry.GetAbsoluteSize().Y,
				WidgetGeometry.GetAbsolutePosition().X,
				WidgetGeometry.GetAbsolutePosition().Y);
			return false;
		}

		const FIntPoint ViewportSize = RawViewport->GetSizeXY();
		if (ViewportSize.X <= 0 || ViewportSize.Y <= 0)
		{
			OutError = CaptureFailure(TEXT("blueprint_scene_viewport_zero_size"), TEXT("The native editor viewport has a non-positive capture width or height."), ViewportWidget->GetTypeAsString()); OutError.PropertyPath = TEXT("viewport_size"); OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("positive width and height")); OutError.ActualValue = MakeShared<FJsonValueString>(ViewportSize.ToString());
			const FGeometry& WidgetGeometry = ViewportWidget->GetTickSpaceGeometry();
			FUeAgentInterfaceLogger::Log(
				TEXT("ScreenshotEditorViewportPixels zero-size raw_viewport=(%d,%d) widget_local=(%.1f,%.1f) widget_abs=(%.1f,%.1f) widget_pos=(%.1f,%.1f)"),
				ViewportSize.X,
				ViewportSize.Y,
				WidgetGeometry.GetLocalSize().X,
				WidgetGeometry.GetLocalSize().Y,
				WidgetGeometry.GetAbsoluteSize().X,
				WidgetGeometry.GetAbsoluteSize().Y,
				WidgetGeometry.GetAbsolutePosition().X,
				WidgetGeometry.GetAbsolutePosition().Y);
			return false;
		}

		FReadSurfaceDataFlags ReadFlags(RCM_UNorm);
		ReadFlags.SetLinearToGamma(true);
		OutPixels.Reset();
		if (!RawViewport->ReadPixels(OutPixels, ReadFlags))
		{
			OutError = CaptureFailure(TEXT("blueprint_scene_viewport_read_pixels_failed"), TEXT("Reading pixels from the native editor viewport failed."), ViewportWidget->GetTypeAsString()); OutError.ActualValue = MakeShared<FJsonValueString>(ViewportSize.ToString());
			return false;
		}

		if (OutPixels.Num() <= 0)
		{
			OutError = CaptureFailure(TEXT("blueprint_scene_viewport_empty_pixels"), TEXT("The native editor viewport returned an empty pixel buffer."), ViewportWidget->GetTypeAsString()); OutError.PropertyPath = TEXT("pixel_count"); OutError.ExpectedValue = MakeShared<FJsonValueNumber>(static_cast<int64>(ViewportSize.X) * ViewportSize.Y); OutError.ActualValue = MakeShared<FJsonValueNumber>(0);
			return false;
		}

		OutSize = ViewportSize;
		return true;
	}

	bool ScreenshotBlueprintWindow(
		const TSharedPtr<IBlueprintEditor>& BlueprintEditor,
		TArray<FColor>& OutPixels,
		FIntPoint& OutSize,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		return ScreenshotBlueprintWindow(BlueprintEditor, OutPixels, OutSize, OutError, true, nullptr);
	}

	bool ScreenshotBlueprintWindow(
		const TSharedPtr<IBlueprintEditor>& BlueprintEditor,
		TArray<FColor>& OutPixels,
		FIntPoint& OutSize,
		UE::UAI::Contract::FDiagnostic& OutError,
		const bool bAllowWindowMutation,
		bool* OutWindowMutated)
	{
		const TSharedPtr<SWindow> BlueprintWindow = ResolveBlueprintEditorWindow(BlueprintEditor, OutError);
		if (!BlueprintWindow.IsValid())
		{
			return false;
		}
		if (!UE::UAI::EditorCapture::SlateWindow::Prepare(BlueprintWindow, OutError, bAllowWindowMutation, OutWindowMutated))
		{
			return false;
		}
		return ScreenshotSlateWidget(BlueprintWindow.ToSharedRef(), OutPixels, OutSize, OutError);
	}

	static bool BuildWidgetCropRectInWindowPixels(
		const TSharedRef<SWindow>& Window,
		const TSharedRef<SWidget>& Widget,
		const FIntPoint& WindowShotSize,
		FIntRect& OutRect,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		const FGeometry& WindowGeometry = Window->GetTickSpaceGeometry();
		const FGeometry& WidgetGeometry = Widget->GetTickSpaceGeometry();

		const FVector2D WindowLocalSize = WindowGeometry.GetLocalSize();
		const FVector2D WidgetAbsPos = WidgetGeometry.GetAbsolutePosition();
		const FVector2D WidgetAbsSize = WidgetGeometry.GetAbsoluteSize();
		const FVector2D LocalTopLeft = WindowGeometry.AbsoluteToLocal(WidgetAbsPos);
		const FVector2D LocalBottomRight = WindowGeometry.AbsoluteToLocal(WidgetAbsPos + WidgetAbsSize);

		if (WindowLocalSize.X <= 1.0 || WindowLocalSize.Y <= 1.0)
		{
			OutError = CaptureFailure(TEXT("blueprint_window_geometry_invalid"), TEXT("The capture window's local width or height is too small to compute the widget crop."), Window->GetTitle().ToString()); OutError.PropertyPath = TEXT("window_local_size"); OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("width and height > 1")); OutError.ActualValue = MakeShared<FJsonValueString>(WindowLocalSize.ToString());
			return false;
		}
		if (WidgetAbsSize.X <= 1.0 || WidgetAbsSize.Y <= 1.0)
		{
			OutError = CaptureFailure(TEXT("blueprint_widget_geometry_invalid"), TEXT("The widget's absolute width or height is too small to compute its capture crop."), Widget->GetTypeAsString()); OutError.PropertyPath = TEXT("widget_absolute_size"); OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("width and height > 1")); OutError.ActualValue = MakeShared<FJsonValueString>(WidgetAbsSize.ToString());
			return false;
		}

		const double ScaleX = (double)WindowShotSize.X / (double)WindowLocalSize.X;
		const double ScaleY = (double)WindowShotSize.Y / (double)WindowLocalSize.Y;
		int32 Left = FMath::FloorToInt(LocalTopLeft.X * ScaleX);
		int32 Top = FMath::FloorToInt(LocalTopLeft.Y * ScaleY);
		int32 Right = FMath::CeilToInt(LocalBottomRight.X * ScaleX);
		int32 Bottom = FMath::CeilToInt(LocalBottomRight.Y * ScaleY);

		Left = FMath::Clamp(Left, 0, WindowShotSize.X - 1);
		Top = FMath::Clamp(Top, 0, WindowShotSize.Y - 1);
		Right = FMath::Clamp(Right, Left + 1, WindowShotSize.X);
		Bottom = FMath::Clamp(Bottom, Top + 1, WindowShotSize.Y);

		if (Right <= Left || Bottom <= Top)
		{
			OutError = CaptureFailure(TEXT("blueprint_widget_crop_invalid"), TEXT("The computed widget crop has no positive width or height after clipping to the window image."), Widget->GetTypeAsString()); OutError.PropertyPath = TEXT("crop_rect"); OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("right > left and bottom > top")); OutError.ActualValue = MakeShared<FJsonValueString>(FIntRect(Left, Top, Right, Bottom).ToString());
			return false;
		}

		FUeAgentInterfaceLogger::Log(
			TEXT("BuildWidgetCropRectInWindowPixels window_local=(%.1f,%.1f) window_shot=(%d,%d) widget_abs_pos=(%.1f,%.1f) widget_abs_size=(%.1f,%.1f) crop=(%d,%d)-(%d,%d)"),
			WindowLocalSize.X,
			WindowLocalSize.Y,
			WindowShotSize.X,
			WindowShotSize.Y,
			WidgetAbsPos.X,
			WidgetAbsPos.Y,
			WidgetAbsSize.X,
			WidgetAbsSize.Y,
			Left,
			Top,
			Right,
			Bottom);

		OutRect = FIntRect(Left, Top, Right, Bottom);
		return true;
	}

	static bool CropPixelsRect(
		const TArray<FColor>& InPixels,
		const FIntPoint& InSize,
		const FIntRect& CropRect,
		TArray<FColor>& OutPixels,
		FIntPoint& OutSize,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (InSize.X <= 0 || InSize.Y <= 0 || InPixels.Num() != static_cast<int64>(InSize.X) * InSize.Y)
		{
			OutError = CaptureFailure(TEXT("invalid_source_pixels"), InSize.X <= 0 || InSize.Y <= 0 ? TEXT("The source image has a non-positive width or height.") : TEXT("The source pixel count does not match the image dimensions."));
			if (InSize.X <= 0 || InSize.Y <= 0) { OutError.PropertyPath = TEXT("source_size"); OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("positive width and height")); OutError.ActualValue = MakeShared<FJsonValueString>(InSize.ToString()); }
			else { OutError.PropertyPath = TEXT("pixel_count"); OutError.ExpectedValue = MakeShared<FJsonValueNumber>(static_cast<int64>(InSize.X) * InSize.Y); OutError.ActualValue = MakeShared<FJsonValueNumber>(InPixels.Num()); }
			return false;
		}

		if (CropRect.Min.X < 0 || CropRect.Min.Y < 0
			|| CropRect.Max.X > InSize.X || CropRect.Max.Y > InSize.Y
			|| CropRect.Max.X <= CropRect.Min.X || CropRect.Max.Y <= CropRect.Min.Y)
		{
			OutError = CaptureFailure(TEXT("invalid_crop_rect"), CropRect.Max.X <= CropRect.Min.X || CropRect.Max.Y <= CropRect.Min.Y ? TEXT("The requested crop rectangle has no positive width or height.") : TEXT("The requested crop rectangle extends outside the source image.")); OutError.PropertyPath = TEXT("crop_rect"); OutError.ExpectedValue = MakeShared<FJsonValueString>(FIntRect(FIntPoint::ZeroValue, InSize).ToString()); OutError.ActualValue = MakeShared<FJsonValueString>(CropRect.ToString());
			return false;
		}

		const int32 CropWidth = CropRect.Width();
		const int32 CropHeight = CropRect.Height();
		OutPixels.Reset();
		OutPixels.AddUninitialized(CropWidth * CropHeight);
		for (int32 Row = 0; Row < CropHeight; ++Row)
		{
			const int32 SourceIndex = (CropRect.Min.Y + Row) * InSize.X + CropRect.Min.X;
			const int32 DestIndex = Row * CropWidth;
			FMemory::Memcpy(
				OutPixels.GetData() + DestIndex,
				InPixels.GetData() + SourceIndex,
				sizeof(FColor) * CropWidth);
		}

		OutSize = FIntPoint(CropWidth, CropHeight);
		return true;
	}

	bool ScreenshotBlueprintWindowCropWidget(
		const TSharedPtr<IBlueprintEditor>& BlueprintEditor,
		const TSharedRef<SWidget>& Widget,
		TArray<FColor>& OutPixels,
		FIntPoint& OutSize,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		return ScreenshotBlueprintWindowCropWidget(BlueprintEditor, Widget, OutPixels, OutSize, OutError, true, nullptr);
	}

	bool ScreenshotBlueprintWindowCropWidget(
		const TSharedPtr<IBlueprintEditor>& BlueprintEditor,
		const TSharedRef<SWidget>& Widget,
		TArray<FColor>& OutPixels,
		FIntPoint& OutSize,
		UE::UAI::Contract::FDiagnostic& OutError,
		const bool bAllowWindowMutation,
		bool* OutWindowMutated)
	{
		const TSharedPtr<SWindow> BlueprintWindow = ResolveBlueprintEditorWindow(BlueprintEditor, OutError);
		if (!BlueprintWindow.IsValid())
		{
			return false;
		}

		bool bAnyWindowMutated = false;
		bool bWindowMutatedThisPass = false;
		if (!UE::UAI::EditorCapture::SlateWindow::Prepare(BlueprintWindow, OutError, bAllowWindowMutation, &bWindowMutatedThisPass))
		{
			if (OutWindowMutated)
			{
				*OutWindowMutated = bAnyWindowMutated || bWindowMutatedThisPass;
			}
			return false;
		}
		bAnyWindowMutated = bAnyWindowMutated || bWindowMutatedThisPass;

		TArray<FColor> WindowPixels;
		FIntPoint WindowSize(0, 0);
		if (!ScreenshotSlateWidget(BlueprintWindow.ToSharedRef(), WindowPixels, WindowSize, OutError))
		{
			if (OutWindowMutated)
			{
				*OutWindowMutated = bAnyWindowMutated;
			}
			return false;
		}

		FUeAgentInterfaceLogger::Log(
			TEXT("ScreenshotBlueprintWindowCropWidget warmup window_shot=(%d,%d)"),
			WindowSize.X,
			WindowSize.Y);

		bWindowMutatedThisPass = false;
		if (!UE::UAI::EditorCapture::SlateWindow::Prepare(BlueprintWindow, OutError, bAllowWindowMutation, &bWindowMutatedThisPass))
		{
			if (OutWindowMutated)
			{
				*OutWindowMutated = bAnyWindowMutated || bWindowMutatedThisPass;
			}
			return false;
		}
		bAnyWindowMutated = bAnyWindowMutated || bWindowMutatedThisPass;
		if (!ScreenshotSlateWidget(BlueprintWindow.ToSharedRef(), WindowPixels, WindowSize, OutError))
		{
			if (OutWindowMutated)
			{
				*OutWindowMutated = bAnyWindowMutated;
			}
			return false;
		}

		FIntRect CropRect;
		if (!BuildWidgetCropRectInWindowPixels(BlueprintWindow.ToSharedRef(), Widget, WindowSize, CropRect, OutError))
		{
			if (OutWindowMutated)
			{
				*OutWindowMutated = bAnyWindowMutated;
			}
			return false;
		}

		if (OutWindowMutated)
		{
			*OutWindowMutated = bAnyWindowMutated;
		}
		return CropPixelsRect(WindowPixels, WindowSize, CropRect, OutPixels, OutSize, OutError);
	}
}
