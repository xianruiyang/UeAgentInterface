#include "Core/AssetPreview/UeAgentAssetPreviewState.h"

#include "Blueprint/UserWidget.h"
#include "Core/EditorCapture/UeAgentEditorWidgetTree.h"
#include "Designer/SDesignerView.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Framework/Docking/TabManager.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "WidgetBlueprint.h"
#include "Widgets/Docking/SDockTab.h"

namespace UE::UAI::AssetPreview
{
	namespace
	{
		const UUserWidget* Defaults(const FContext& Context)
		{
			const auto* Blueprint = Cast<UWidgetBlueprint>(Context.Asset.Get());
			return Blueprint && Blueprint->GeneratedClass ? Cast<UUserWidget>(Blueprint->GeneratedClass->GetDefaultObject()) : nullptr;
		}
		bool SizeWritable(const FContext& Context)
		{
			const auto* Widget = Defaults(Context);
			return Widget && Widget->DesignSizeMode != EDesignPreviewSizeMode::Custom
				&& Widget->DesignSizeMode != EDesignPreviewSizeMode::CustomOnScreen;
		}
	}

	bool IsWidgetPreview(const UObject* Asset) { return Asset && Asset->IsA<UWidgetBlueprint>(); }

	bool ResolveWidgetContext(UObject* Asset, bool bOpen, FContext& Out, UE::UAI::Contract::FDiagnostic& OutError)
	{
		Out.bWidgetPreview = true;
		Out.ViewportType = TEXT("SDesignerView");
		auto* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
		if (!Subsystem) { OutError = PreviewFailure(TEXT("widget_preview_editor_unavailable"), TEXT("The asset editor subsystem required for the widget designer preview is unavailable."), Asset); return false; }
		auto* Editor = Subsystem->FindEditorForAsset(Asset, bOpen);
		if (!Editor && bOpen && OpenAssetEditor(Asset)) Editor = Subsystem->FindEditorForAsset(Asset, false);
		Out.Asset = Asset;
		const auto Tabs = Editor ? Editor->GetAssociatedTabManager() : nullptr;
		if (Tabs)
		{
			auto Tab = Tabs->FindExistingLiveTab(FTabId(TEXT("SlatePreview")));
			if (!Tab && bOpen) Tab = Tabs->TryInvokeTab(FTabId(TEXT("SlatePreview")), false);
			if (Tab && bOpen) Tab->ActivateInParent(ETabActivationCause::SetDirectly);
			if (Tab)
			{
				for (const FName Type : {FName(TEXT("SDesignerView")), FName(TEXT("TToolCompatibleMixin<SDesignerView>"))})
					if (EditorCapture::WidgetTree::FindFirstByType(Tab->GetContent(), Type, Out.PreviewWidget))
					{ Out.Source = TEXT("live"); return true; }
			}
		}
		if (bOpen) { OutError = PreviewFailure(TEXT("widget_designer_preview_unavailable"), !Editor ? TEXT("The widget editor could not be opened.") : !Tabs ? TEXT("The widget editor has no tab manager.") : TEXT("The widget editor's preview tab has no available designer view."), Asset); return false; }
		Out.Source = TEXT("defaults");
		return true;
	}

	bool ReadWidgetState(const FContext& Context, FLocalState& State)
	{
		State.bWidgetPreview = true;
		State.bWidgetSizeWritable = SizeWritable(Context);
		if (!Context.PreviewWidget) return false;
		const auto Designer = StaticCastSharedPtr<SDesignerView>(Context.PreviewWidget);
		State.Widget.Width = FMath::RoundToInt(Designer->GetPreviewAreaWidth().Get());
		State.Widget.Height = FMath::RoundToInt(Designer->GetPreviewAreaHeight().Get());
		State.bHasView = true;
		return true;
	}

	bool ApplyWidgetState(const FContext& Context, const FUeAgentAssetPreviewWidget& Value, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!Context.PreviewWidget || !SizeWritable(Context))
		{ OutError = PreviewFailure(TEXT("widget_preview_size_owned_by_asset"), !Context.PreviewWidget ? TEXT("No live widget designer preview is available to resize.") : !Defaults(Context) ? TEXT("Widget class defaults are unavailable, so preview size ownership cannot be determined.") : TEXT("This widget uses an asset-owned custom design size; change the asset design size instead of the preview area."), Context.Asset.Get(), Context.Target); return false; }
		if (Value.Width < 1 || Value.Height < 1) {
			OutError = PreviewValueFailure(TEXT("widget_preview_size_invalid"), TEXT("The widget preview dimension must be at least one pixel."), Value.Width < 1 ? TEXT("Width") : TEXT("Height"),
				MakeShared<FJsonValueString>(TEXT("integer >= 1")), PreviewNumber(Value.Width < 1 ? Value.Width : Value.Height), Context.Asset.Get(), Context.Target); return false;
		}
		StaticCastSharedPtr<SDesignerView>(Context.PreviewWidget)->SetPreviewAreaSize(Value.Width, Value.Height);
		return true;
	}

	void ReadWidgetStatus(const FContext& Context, FJsonObject& Out)
	{
		if (!Context.PreviewWidget) return;
		const auto Designer = StaticCastSharedPtr<SDesignerView>(Context.PreviewWidget);
		auto State = MakeShared<FJsonObject>();
		State->SetNumberField(TEXT("area_width"), Designer->GetPreviewAreaWidth().Get());
		State->SetNumberField(TEXT("area_height"), Designer->GetPreviewAreaHeight().Get());
		State->SetNumberField(TEXT("widget_width"), Designer->GetPreviewSizeWidth().Get());
		State->SetNumberField(TEXT("widget_height"), Designer->GetPreviewSizeHeight().Get());
		State->SetNumberField(TEXT("dpi_scale"), Designer->GetPreviewDPIScale());
		State->SetNumberField(TEXT("view_zoom"), Designer->GetZoomAmount());
		State->SetBoolField(TEXT("size_writable"), SizeWritable(Context));
		if (!SizeWritable(Context)) State->SetStringField(TEXT("size_scope"), TEXT("asset_default"));
		Out.SetObjectField(TEXT("widget_readback"), State);
	}
}
