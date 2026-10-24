#include "Core/AssetPreview/UeAgentAssetPreviewState.h"

#include "Core/EditorCapture/UeAgentEditorWidgetTree.h"
#include "Editor.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Texture.h"
#include "Framework/Commands/InputBindingManager.h"
#include "Framework/Commands/UICommandList.h"
#include "Framework/Docking/TabManager.h"
#include "Interfaces/ITextureEditorToolkit.h"
#include "Slate/SceneViewport.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SViewport.h"

namespace UE::UAI::AssetPreview
{
	namespace
	{
		ITextureEditorToolkit* Toolkit(UObject* Asset)
		{
			auto* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
			auto* Editor = Subsystem && Asset && Asset->IsA<UTexture>() ? Subsystem->FindEditorForAsset(Asset, false) : nullptr;
			return Editor && Editor->GetEditorName() == TEXT("TextureEditor") ? static_cast<ITextureEditorToolkit*>(Editor) : nullptr;
		}

		TSharedPtr<FUICommandInfo> Command(FName Name)
		{
			return FInputBindingManager::Get().FindCommandInContext(TEXT("TextureEditor"), Name);
		}

		bool HasChannelReadback(ITextureEditorToolkit& Editor)
		{
			const auto Mode = Editor.GetColourChannelBlendMode();
			return Mode >= SE_BLEND_RGBA_MASK_START && Mode <= SE_BLEND_RGBA_MASK_END;
		}

		bool ReadChannel(ITextureEditorToolkit& Editor, FName Name)
		{
			// Native channel commands have no check delegate. Read the actual
			// renderer mask rather than the command list's default unchecked state.
			if (!HasChannelReadback(Editor)) return false;
			const uint32 Mask = static_cast<uint32>(Editor.GetColourChannelBlendMode()) - SE_BLEND_RGBA_MASK_START;
			const uint32 Bit = Name == TEXT("RedChannel") ? 0 : Name == TEXT("GreenChannel") ? 1
				: Name == TEXT("BlueChannel") ? 2 : 3;
			return (Mask & (1u << Bit)) != 0;
		}

		bool ApplyChannel(ITextureEditorToolkit& Editor, FName Name, bool Value, UE::UAI::Contract::FDiagnostic& Error)
		{
			const auto Info = Command(Name);
			if (!Info || !HasChannelReadback(Editor)) {
				Error = PreviewFailure(TEXT("texture_preview_channel_apply_failed"), !Info ? TEXT("The native texture channel command is not registered.") : TEXT("The current texture blend mode does not expose a readable RGBA channel mask."), nullptr, Name.ToString());
				if (Info) Error.ActualValue = PreviewNumber(static_cast<int32>(Editor.GetColourChannelBlendMode()));
				return false;
			}
			if (ReadChannel(Editor, Name) == Value) return true;
			const bool bExecuted = Editor.GetToolkitCommands()->TryExecuteAction(Info.ToSharedRef());
			const bool Actual = ReadChannel(Editor, Name);
			if (bExecuted && Actual == Value) return true;
			Error = PreviewFailure(TEXT("texture_preview_channel_apply_failed"), !bExecuted ? TEXT("The texture editor rejected the channel toggle.") : TEXT("The channel toggle completed without reaching the requested channel state."), nullptr, Name.ToString());
			Error.ExpectedValue = MakeShared<FJsonValueBoolean>(Value); Error.ActualValue = MakeShared<FJsonValueBoolean>(Actual); return false;
		}
	}

	bool ResolveImageContext(UObject* Asset, bool bOpen, FContext& Out, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (bOpen && !Toolkit(Asset) && !OpenAssetEditor(Asset))
		{ OutError = PreviewFailure(TEXT("texture_preview_open_failed"), TEXT("The asset editor could not be opened for texture preview."), Asset); return false; }
		Out.Asset = Asset;
		Out.AdvancedScene = nullptr;
		Out.ProfileIndex = INDEX_NONE;
		Out.ViewportType = TEXT("STextureEditorViewport");
		auto* Editor = Toolkit(Asset);
		const auto Tabs = Editor ? Editor->GetAssociatedTabManager() : nullptr;
		if (Tabs)
		{
			auto Tab = Tabs->FindExistingLiveTab(FTabId(TEXT("TextureEditor_Viewport")));
			if (!Tab && bOpen) Tab = Tabs->TryInvokeTab(FTabId(TEXT("TextureEditor_Viewport")), false);
			if (Tab && bOpen) Tab->ActivateInParent(ETabActivationCause::SetDirectly);
			TSharedPtr<SWidget> TextureWidget, ViewportWidget;
			if (Tab && EditorCapture::WidgetTree::FindFirstByType(Tab->GetContent(), TEXT("STextureEditorViewport"), TextureWidget)
				&& EditorCapture::WidgetTree::FindFirstByType(TextureWidget.ToSharedRef(), TEXT("SViewport"), ViewportWidget))
			{
				const auto Interface = StaticCastSharedPtr<SViewport>(ViewportWidget)->GetViewportInterface().Pin();
				// STextureEditorViewport installs exactly FSceneViewport in this SViewport.
				Out.ImageViewport = StaticCastSharedPtr<FSceneViewport>(Interface);
				Out.PreviewWidget = TextureWidget;
				if (Out.ImageViewport) { Out.Source = TEXT("live"); return true; }
			}
		}
		if (bOpen) { OutError = PreviewFailure(TEXT("texture_preview_viewport_unavailable"), !Editor ? TEXT("The texture asset has no available texture editor toolkit.") : !Tabs ? TEXT("The texture editor has no tab manager.") : TEXT("The texture preview tab has no available native scene viewport."), Asset); return false; }
		Out.Source = TEXT("defaults");
		return true;
	}

	bool ReadImageState(const FContext& Context, FLocalState& State)
	{
		auto* Editor = Toolkit(Context.Asset.Get());
		if (!Editor || !Context.ImageViewport) return false;
		State.ImageView.ZoomMode = Editor->GetZoomMode();
		State.ImageView.CustomZoom = Editor->GetCustomZoomLevel();
		State.ImageView.Orientation = Editor->GetOrientation();
		State.Image.Sampling = Editor->GetSampling();
		State.Image.VolumeOpacity = Editor->GetVolumeOpacity();
		State.Image.VolumeViewMode = Editor->GetVolumeViewMode();
		State.Image.CubemapViewMode = Editor->GetCubemapViewMode();
		State.Image.Red = ReadChannel(*Editor, TEXT("RedChannel"));
		State.Image.Green = ReadChannel(*Editor, TEXT("GreenChannel"));
		State.Image.Blue = ReadChannel(*Editor, TEXT("BlueChannel"));
		State.Image.Alpha = ReadChannel(*Editor, TEXT("AlphaChannel"));
		State.bHasView = true;
		State.bImagePreview = true;
		return true;
	}

	bool IsImageFieldSupported(const FContext& Context, const FString& Group, FName Field)
	{
		auto* Editor = Toolkit(Context.Asset.Get());
		if (!Editor) return false;
		if (Field == TEXT("Red") || Field == TEXT("Green") || Field == TEXT("Blue") || Field == TEXT("Alpha"))
			return HasChannelReadback(*Editor);
		if (Field == TEXT("Orientation")) return Editor->IsUsingOrientation();
		if (Field == TEXT("VolumeOpacity") || Field == TEXT("VolumeViewMode")) return Editor->IsVolumeTexture();
		if (Field == TEXT("CubemapViewMode"))
			return Context.Asset.IsValid() && Context.Asset->GetClass()->GetName().StartsWith(TEXT("TextureCube"));
		return true;
	}

	void ReadImageStatus(const FContext& Context, FJsonObject& Out)
	{
		if (auto* Editor = Toolkit(Context.Asset.Get()))
		{
			auto ReadOnly = MakeShared<FJsonObject>();
			ReadOnly->SetNumberField(TEXT("mip"), Editor->GetMipLevel());
			ReadOnly->SetNumberField(TEXT("layer"), Editor->GetLayer());
			ReadOnly->SetNumberField(TEXT("slice"), Editor->GetSlice());
			ReadOnly->SetNumberField(TEXT("face"), Editor->GetFace());
			ReadOnly->SetNumberField(TEXT("exposure_bias"), Editor->GetExposureBias());
			ReadOnly->SetNumberField(TEXT("effective_zoom"), Editor->CalculateDisplayedZoomLevel());
			Out.SetObjectField(TEXT("image_readback"), ReadOnly);
		}
	}

	FString ImageFieldScope(const FString& Group, FName Field)
	{
		return Field == TEXT("ZoomMode") || Field == TEXT("VolumeViewMode")
			|| Field == TEXT("CubemapViewMode") || Field == TEXT("Sampling")
			? TEXT("viewport_and_user_defaults") : TEXT("viewport");
	}

	bool ApplyImageState(const FContext& Context, FLocalState& State, const FString& Group,
		const TSet<FName>& Fields, UE::UAI::Contract::FDiagnostic& OutError)
	{
		auto* Editor = Toolkit(Context.Asset.Get());
		if (!Editor) { OutError = PreviewFailure(TEXT("texture_preview_owner_unavailable"), TEXT("No open texture editor owns this preview configuration."), Context.Asset.Get(), Context.Target); return false; }
		if (Group == TEXT("view"))
		{
			if (!FMath::IsFinite(State.ImageView.CustomZoom) || State.ImageView.CustomZoom <= 0
				|| State.ImageView.Orientation.ContainsNaN())
			{
				const bool bZoomInvalid = !FMath::IsFinite(State.ImageView.CustomZoom) || State.ImageView.CustomZoom <= 0;
				OutError = PreviewValueFailure(TEXT("texture_preview_view_invalid"), bZoomInvalid ? TEXT("Texture preview custom zoom must be finite and positive.") : TEXT("Texture preview orientation must have finite pitch, yaw and roll components."),
					bZoomInvalid ? TEXT("CustomZoom") : TEXT("Orientation"), MakeShared<FJsonValueString>(bZoomInvalid ? TEXT("finite number > 0") : TEXT("finite rotation components")),
					bZoomInvalid ? PreviewNumber(State.ImageView.CustomZoom) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(State.ImageView.Orientation.ToString())), Context.Asset.Get(), Context.Target); return false;
			}
			if (Fields.Contains(TEXT("CustomZoom"))) Editor->SetCustomZoomLevel(State.ImageView.CustomZoom);
			// Native SetCustomZoomLevel also switches modes. Preserve an unchanged mode.
			if (Fields.Contains(TEXT("CustomZoom")) || Fields.Contains(TEXT("ZoomMode"))) Editor->SetZoomMode(State.ImageView.ZoomMode);
			if (Fields.Contains(TEXT("Orientation"))) Editor->SetOrientation(State.ImageView.Orientation);
		}
		else if (Group == TEXT("image"))
		{
			if (!FMath::IsFinite(State.Image.VolumeOpacity) || State.Image.VolumeOpacity < 0 || State.Image.VolumeOpacity > 1)
			{ OutError = PreviewValueFailure(TEXT("texture_preview_opacity_invalid"), TEXT("Texture volume opacity must be finite and between zero and one."), TEXT("VolumeOpacity"), MakeShared<FJsonValueString>(TEXT("0 <= opacity <= 1")), PreviewNumber(State.Image.VolumeOpacity), Context.Asset.Get(), Context.Target); return false; }
			for (const auto& Pair : TArray<TPair<FName, bool>>{{TEXT("Red"), State.Image.Red},
				{TEXT("Green"), State.Image.Green}, {TEXT("Blue"), State.Image.Blue}, {TEXT("Alpha"), State.Image.Alpha}})
				if (Fields.Contains(Pair.Key) && !ApplyChannel(*Editor, FName(*(Pair.Key.ToString() + TEXT("Channel"))), Pair.Value, OutError))
				{ OutError.PropertyPath = Pair.Key.ToString(); OutError.TargetPath = State.AssetPath; return false; }
			if (Fields.Contains(TEXT("Sampling")))
			{
				const TCHAR* CommandName = State.Image.Sampling == TextureEditorSampling_Point ? TEXT("PointSampling") : TEXT("DefaultSampling");
				const auto Info = Command(CommandName);
				if (!Info || !Editor->GetToolkitCommands()->TryExecuteAction(Info.ToSharedRef()))
				{
					OutError = PreviewValueFailure(TEXT("texture_preview_sampling_apply_failed"),
						Info ? FString::Printf(TEXT("The texture preview command '%s' rejected execution."), CommandName)
							: FString::Printf(TEXT("The texture preview command '%s' is not registered."), CommandName),
						TEXT("Sampling/command_executed"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), Context.Asset.Get(), Context.Target);
					return false;
				}
			}
			if (Fields.Contains(TEXT("VolumeOpacity"))) Editor->SetVolumeOpacity(State.Image.VolumeOpacity);
			if (Fields.Contains(TEXT("VolumeViewMode"))) Editor->SetVolumeViewMode(State.Image.VolumeViewMode);
			if (Fields.Contains(TEXT("CubemapViewMode"))) Editor->SetCubemapViewMode(State.Image.CubemapViewMode);
		}
		else
		{
			OutError = PreviewValueFailure(TEXT("texture_preview_group_unsupported"),
				TEXT("The requested texture preview configuration group is unsupported."), TEXT("group"),
				MakeShared<FJsonValueString>(TEXT("view or image")), MakeShared<FJsonValueString>(Group), Context.Asset.Get(), Context.Target);
			return false;
		}
		Context.ImageViewport->Invalidate();
		return ReadImageState(Context, State);
	}
}
