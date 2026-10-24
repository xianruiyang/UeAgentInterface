#include "DomainAdapters/CaptureMediaLongJob/UeAgentAssetEditorImageCapture.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureMediaDiagnostics.h"
#include "Misc/ScopeExit.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureImageProcessing.h"
#include "Core/AssetPreview/UeAgentAssetPreviewState.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "GenericPlatform/GenericWindow.h"
#include "Misc/App.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Slate/WidgetRenderer.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/IToolkit.h"
#include "UeAgentInterfaceSettings.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SWindow.h"

namespace UE::UAI::DomainAdapters::CaptureMediaLongJob
{
	using namespace CaptureDiagnostics;
	namespace
	{
		FString ToObjectPath(const FString& InputPath)
		{
			FString ObjectPath = InputPath;
			ObjectPath.TrimStartAndEndInline();
			if (ObjectPath.IsEmpty() || ObjectPath.Contains(TEXT(".")))
			{
				return ObjectPath;
			}
			const FString AssetName = FPackageName::GetLongPackageAssetName(ObjectPath);
			return AssetName.IsEmpty()
				? FString()
				: FString::Printf(TEXT("%s.%s"), *ObjectPath, *AssetName);
		}

		UObject* LoadAsset(const FString& AssetPath)
		{
			const FString ObjectPath = ToObjectPath(AssetPath);
			if (ObjectPath.IsEmpty())
			{
				return nullptr;
			}
			if (UObject* Existing = FindObject<UObject>(nullptr, *ObjectPath))
			{
				return Existing;
			}
			return LoadObject<UObject>(nullptr, *ObjectPath);
		}

		bool ResolveAssetEditorWindow(
			UObject* Asset,
			const bool bOpenIfNeeded,
			TSharedPtr<SWindow>& OutWindow,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutWindow.Reset();
			if (!GEditor)
			{
				OutError = Native(TEXT("editor_unavailable"), TEXT("Asset editor capture requires an active Editor."),
					TEXT("asset_capture"), TEXT("editor_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				return false;
			}
			UAssetEditorSubsystem* AssetEditors =
				GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
			if (!AssetEditors)
			{
				OutError = Native(TEXT("asset_editor_subsystem_unavailable"), TEXT("The Editor asset-editor subsystem is unavailable."),
					TEXT("asset_capture"), TEXT("asset_editor_subsystem_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				return false;
			}
			if (bOpenIfNeeded)
			{
				if (!AssetPreview::OpenAssetEditor(Asset))
				{
					OutError = Native(TEXT("asset_editor_open_failed"), TEXT("The Editor could not open the requested asset editor."),
						TEXT("asset_capture"), TEXT("asset_editor_opened"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return false;
				}
			}
			IAssetEditorInstance* AssetEditor =
				AssetEditors->FindEditorForAsset(Asset, false);
			if (!AssetEditor)
			{
				OutError = Native(bOpenIfNeeded ? TEXT("asset_editor_open_failed") : TEXT("asset_editor_not_opened"),
					TEXT("No asset editor is available for capture."), TEXT("asset_capture"), TEXT("asset_editor_available"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				return false;
			}
			const TSharedPtr<FTabManager> TabManager =
				AssetEditor->GetAssociatedTabManager();
			if (!TabManager.IsValid())
			{
				OutError = Native(TEXT("asset_editor_tab_manager_not_found"), TEXT("The asset editor has no associated tab manager."),
					TEXT("asset_capture"), TEXT("tab_manager_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				return false;
			}
			const TSharedPtr<SDockTab> OwnerTab = TabManager->GetOwnerTab();
			if (!OwnerTab.IsValid())
			{
				OutError = Native(TEXT("asset_editor_owner_tab_not_found"), TEXT("The asset editor tab manager has no owner tab."),
					TEXT("asset_capture"), TEXT("owner_tab_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				return false;
			}
			OutWindow = OwnerTab->GetParentWindow();
			if (!OutWindow.IsValid())
			{
				OutError = Native(TEXT("asset_editor_window_not_found"), TEXT("The asset editor tab has no parent window."),
					TEXT("asset_capture"), TEXT("parent_window_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				return false;
			}
			return true;
		}

		FVector2D ResolveDrawSize(const TSharedPtr<SWindow>& Window)
		{
			FVector2D DrawSize = Window->GetClientSizeInScreen();
			if (DrawSize.X <= 0.0 || DrawSize.Y <= 0.0)
			{
				DrawSize = Window->GetCachedGeometry().GetLocalSize();
			}
			if (DrawSize.X <= 0.0 || DrawSize.Y <= 0.0)
			{
				DrawSize = Window->GetDesiredSize();
			}
			if (DrawSize.X <= 0.0 || DrawSize.Y <= 0.0)
			{
				DrawSize = FVector2D(1440.0, 900.0);
			}
			DrawSize.X = FMath::Max(FMath::RoundToDouble(DrawSize.X), 1.0);
			DrawSize.Y = FMath::Max(FMath::RoundToDouble(DrawSize.Y), 1.0);
			return DrawSize;
		}

		bool IsBackbufferCaptureUnsafe(
			const TSharedPtr<SWindow>& Window,
			FString& OutReason)
		{
			OutReason.Reset();
			if (!Window.IsValid())
			{
				OutReason = TEXT("window_invalid");
				return true;
			}
			if (!FSlateApplication::IsInitialized())
			{
				OutReason = TEXT("slate_not_initialized");
				return true;
			}
			if (!FApp::CanEverRender())
			{
				OutReason = TEXT("rendering_not_available");
				return true;
			}
			if (!Window->IsVisible())
			{
				OutReason = TEXT("window_not_visible");
				return true;
			}
			if (Window->IsWindowMinimized())
			{
				OutReason = TEXT("window_minimized");
				return true;
			}
			if (const TSharedPtr<FGenericWindow> NativeWindow =
				Window->GetNativeWindow())
			{
				if (!NativeWindow->IsVisible())
				{
					OutReason = TEXT("native_window_not_visible");
					return true;
				}
				if (NativeWindow->IsMinimized())
				{
					OutReason = TEXT("native_window_minimized");
					return true;
				}
			}
			const FVector2D ClientSize = Window->GetClientSizeInScreen();
			if (ClientSize.X < 64.0 || ClientSize.Y < 64.0)
			{
				OutReason = FString::Printf(
					TEXT("window_too_small:%.0fx%.0f"),
					ClientSize.X,
					ClientSize.Y);
				return true;
			}
			return false;
		}

		bool CaptureWindow(
			const TSharedPtr<SWindow>& Window,
			TArray<FColor>& OutPixels,
			FIntPoint& OutSize,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			if (!Window.IsValid())
			{
				OutError = Native(TEXT("window_not_found"), TEXT("The requested asset editor window is unavailable."),
					TEXT("asset_capture"), TEXT("window_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				return false;
			}
			if (!FSlateApplication::IsInitialized())
			{
				OutError = Native(TEXT("slate_not_initialized"), TEXT("Slate is not initialized for asset editor capture."),
					TEXT("asset_capture"), TEXT("slate_initialized"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				return false;
			}
			if (!FApp::CanEverRender())
			{
				OutError = Native(TEXT("rendering_not_available"), TEXT("Rendering is unavailable for asset editor capture."),
					TEXT("asset_capture"), TEXT("can_render"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				return false;
			}
			FSlateApplication::Get().PumpMessages();
			const float WindowScale =
				FSlateApplication::Get().GetApplicationScale()
				* Window->GetDPIScaleFactor();
			Window->SlatePrepass(WindowScale);
			FSlateApplication::Get().InvalidateAllViewports();

			const FVector2D DrawSize = ResolveDrawSize(Window);
			FWidgetRenderer* WidgetRenderer = new FWidgetRenderer(true, true);
			UTextureRenderTarget2D* RenderTarget =
				FWidgetRenderer::CreateTargetFor(DrawSize, TF_Bilinear, true);
			if (!RenderTarget)
			{
				BeginCleanup(WidgetRenderer);
				OutError = Native(TEXT("offscreen_render_target_create_failed"), TEXT("The asset editor capture render target could not be created."),
					TEXT("asset_capture"), TEXT("render_target"), Facts({ { TEXT("created"), MakeShared<FJsonValueBoolean>(false) }, { TEXT("width"), Number(DrawSize.X) }, { TEXT("height"), Number(DrawSize.Y) } }), MakeShared<FJsonValueString>(TEXT("render target at requested size")));
				return false;
			}
			WidgetRenderer->DrawWidget(
				RenderTarget, Window.ToSharedRef(), 1.0f, DrawSize, 0.0f, false);
			FlushRenderingCommands();
			BeginCleanup(WidgetRenderer);

			FRenderTarget* RenderTargetResource =
				RenderTarget->GameThread_GetRenderTargetResource();
			if (!RenderTargetResource)
			{
				OutError = Native(TEXT("offscreen_render_target_resource_missing"), TEXT("The asset editor render target has no readable render resource."),
					TEXT("asset_capture"), TEXT("render_resource_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				return false;
			}
			FReadSurfaceDataFlags ReadFlags(RCM_UNorm);
			ReadFlags.SetLinearToGamma(false);
			const bool bReadPixels = RenderTargetResource->ReadPixels(OutPixels, ReadFlags);
			if (!bReadPixels || OutPixels.IsEmpty())
			{
				OutError = Native(TEXT("offscreen_read_pixels_failed"), TEXT("The asset editor render target did not return readable pixels."),
					TEXT("asset_capture"), TEXT("pixel_readback"), Facts({ { TEXT("succeeded"), MakeShared<FJsonValueBoolean>(bReadPixels) }, { TEXT("pixel_count"), Number(OutPixels.Num()) } }), MakeShared<FJsonValueString>(TEXT("successful read with at least one pixel")));
				return false;
			}
			OutSize = FIntPoint(RenderTarget->SizeX, RenderTarget->SizeY);
			if (OutSize.X <= 0
				|| OutSize.Y <= 0
				|| OutPixels.Num() < static_cast<int64>(OutSize.X) * OutSize.Y)
			{
				OutError = Native(TEXT("offscreen_screenshot_empty"), TEXT("The asset editor screenshot dimensions or pixel count are invalid."),
					TEXT("asset_capture"), TEXT("image"), Facts({ { TEXT("width"), Number(OutSize.X) }, { TEXT("height"), Number(OutSize.Y) }, { TEXT("pixel_count"), Number(OutPixels.Num()) } }), MakeShared<FJsonValueString>(TEXT("positive dimensions and pixel_count >= width * height")));
				return false;
			}
			return true;
		}

		int32 CountNonBlackPixels(const TArray<FColor>& Pixels)
		{
			int32 Count = 0;
			for (const FColor& Pixel : Pixels)
			{
				const int32 Luma = (static_cast<int32>(Pixel.R)
					+ static_cast<int32>(Pixel.G)
					+ static_cast<int32>(Pixel.B)) / 3;
				if (Luma > 4 || Pixel.A > 4)
				{
					++Count;
				}
			}
			return Count;
		}

		FString MakeDefaultOutputPath(const FString& Extension)
		{
			const UUeAgentInterfaceSettings* Settings =
				UUeAgentInterfaceSettings::Get();
			const FString RootRelative = Settings
				? Settings->ArtifactsDirRelative
				: TEXT("Saved/RemoteArtifacts");
			const FString Directory = FPaths::ConvertRelativePathToFull(
				FPaths::ProjectDir(), RootRelative / TEXT("Shots"));
			return FPaths::Combine(
				Directory,
				FString::Printf(
					TEXT("shot_%s_%s.%s"),
					FPlatformProcess::ComputerName(),
					*FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S_%s")),
					*Extension));
		}
	}

	bool TryCaptureAssetEditorImage(
		const TSharedPtr<FJsonObject>& Parameters,
		const FString& AssetNotFoundError,
		TSharedPtr<FJsonObject>& OutData,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutData.Reset();
		FString AssetPath;
		if (!Parameters.IsValid()
			|| !Parameters->TryGetStringField(TEXT("asset_path"), AssetPath)
			|| AssetPath.IsEmpty())
		{
			OutError = Input(TEXT("missing_asset_path"), TEXT("Asset editor capture requires a non-empty asset_path."),
				TEXT("asset_capture"), TEXT("/asset_path"), Parameters.IsValid() ? Parameters->TryGetField(TEXT("asset_path")) : nullptr,
				MakeShared<FJsonValueString>(TEXT("non-empty asset path")));
			return false;
		}
		ON_SCOPE_EXIT { if (!OutError.IsEmpty() && OutError.TargetPath.IsEmpty()) OutError.TargetPath = AssetPath; };
		UObject* Asset = LoadAsset(AssetPath);
		if (!Asset)
		{
			OutError = Input(AssetNotFoundError.IsEmpty() ? TEXT("asset_not_found") : AssetNotFoundError,
				TEXT("The requested asset could not be loaded."), TEXT("asset_capture"), TEXT("/asset_path"),
				MakeShared<FJsonValueString>(AssetPath), MakeShared<FJsonValueString>(TEXT("loadable asset")));
            OutError.TargetPath = AssetPath;
			return false;
		}

		bool bOpenIfNeeded = true;
		Parameters->TryGetBoolField(TEXT("open_editor_if_needed"), bOpenIfNeeded);
		TSharedPtr<SWindow> Window;
		if (!ResolveAssetEditorWindow(
				Asset, bOpenIfNeeded, Window, OutError))
		{
			return false;
		}
		FString BackbufferUnsafeReason;
		const bool bBackbufferUnsafe =
			IsBackbufferCaptureUnsafe(Window, BackbufferUnsafeReason);
		TArray<FColor> Pixels;
		FIntPoint Size;
		if (!CaptureWindow(Window, Pixels, Size, OutError))
		{
			return false;
		}

		double MaxSizeNumber = 0.0;
		const bool bHasMaxSize = Parameters->TryGetNumberField(TEXT("max_size"), MaxSizeNumber);
		const int32 MaxSize = bHasMaxSize
			? static_cast<int32>(FMath::Max(MaxSizeNumber, 1.0))
			: 0;
		TArray<FColor> ResizedPixels;
		FIntPoint ResizedSize;
		if (!FCaptureImageProcessing::ResizePixelsMaxSize(
				Pixels, Size, MaxSize, ResizedPixels, ResizedSize, OutError))
		{
			return false;
		}

		bool bRequireNonBlack = true;
		Parameters->TryGetBoolField(TEXT("require_non_black"), bRequireNonBlack);
		const int32 NonBlackPixelCount = CountNonBlackPixels(ResizedPixels);
		if (bRequireNonBlack && NonBlackPixelCount <= 0)
		{
			OutError = Native(TEXT("asset_editor_screenshot_black"), TEXT("The asset editor screenshot contains no non-black pixels."),
				TEXT("asset_capture"), TEXT("non_black_pixel_count"), Number(NonBlackPixelCount), MakeShared<FJsonValueString>(TEXT("at least one non-black pixel")));
			return false;
		}

		FString Format = TEXT("png");
		Parameters->TryGetStringField(TEXT("format"), Format);
		Format.TrimStartAndEndInline();
		Format.ToLowerInline();
		if (Format == TEXT("jpeg"))
		{
			Format = TEXT("jpg");
		}
		if (Format != TEXT("png") && Format != TEXT("jpg"))
		{
			OutError = Input(TEXT("unsupported_format"), TEXT("The asset editor capture image format is unsupported."),
				TEXT("asset_capture"), TEXT("/format"), Parameters->TryGetField(TEXT("format")), MakeShared<FJsonValueString>(TEXT("png or jpg/jpeg")));
			return false;
		}
		FString OutputPath;
		Parameters->TryGetStringField(TEXT("output_file"), OutputPath);
		if (OutputPath.TrimStartAndEnd().IsEmpty())
		{
			OutputPath = MakeDefaultOutputPath(Format);
		}
		else
		{
			OutputPath = FPaths::ConvertRelativePathToFull(OutputPath);
		}
		double QualityNumber = 90.0;
		Parameters->TryGetNumberField(TEXT("quality"), QualityNumber);
		const int32 Quality = static_cast<int32>(
			FMath::Clamp(QualityNumber, 1.0, 100.0));
		if (!FCaptureImageProcessing::WriteCompressedImage(
				ResizedPixels,
				ResizedSize,
				Format,
				Quality,
				OutputPath,
				OutError))
		{
			return false;
		}

		double LumaSum = 0.0;
		for (const FColor& Pixel : ResizedPixels)
		{
			LumaSum += (static_cast<int32>(Pixel.R)
				+ static_cast<int32>(Pixel.G)
				+ static_cast<int32>(Pixel.B)) / 3;
		}
		OutData = MakeShared<FJsonObject>();
		OutData->SetStringField(TEXT("asset_path"), Asset->GetOutermost()->GetName());
		OutData->SetStringField(TEXT("file_path"), OutputPath);
		OutData->SetNumberField(TEXT("width"), ResizedSize.X);
		OutData->SetNumberField(TEXT("height"), ResizedSize.Y);
		OutData->SetStringField(TEXT("format"), Format);
		OutData->SetStringField(
			TEXT("capture_mode"), TEXT("offscreen_widget_renderer"));
		OutData->SetStringField(
			TEXT("legacy_backbuffer_capture"), TEXT("disabled"));
		OutData->SetBoolField(
			TEXT("backbuffer_capture_would_be_unsafe"), bBackbufferUnsafe);
		if (!BackbufferUnsafeReason.IsEmpty())
		{
			OutData->SetStringField(
				TEXT("backbuffer_unsafe_reason"), BackbufferUnsafeReason);
		}
		OutData->SetNumberField(TEXT("non_black_pixel_count"), NonBlackPixelCount);
		OutData->SetNumberField(
			TEXT("average_luma"),
			ResizedPixels.IsEmpty()
				? 0.0
				: LumaSum / static_cast<double>(ResizedPixels.Num()));
		OutData->SetBoolField(
			TEXT("appears_non_black"), NonBlackPixelCount > 0);
		OutData->SetStringField(TEXT("status"), TEXT("ok"));
		return true;
	}
}
