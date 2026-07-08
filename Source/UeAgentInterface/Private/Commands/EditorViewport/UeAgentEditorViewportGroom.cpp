// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commands/EditorViewport/UeAgentEditorViewportGroom.h"

#include "Utilities/UeAgentJsonDiagnostics.h"
#include "UeAgentInterfaceSettings.h"
#include "Server/UeAgentHttpServer.h"

#include "Editor.h"
#include "EditorViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "GroomAsset.h"
#include "GroomBindingAsset.h"
#include "GroomComponent.h"
#include "GroomVisualizationData.h"
#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "RenderingThread.h"
#include "SEditorViewport.h"
#include "ShowFlags.h"
#include "Slate/SceneViewport.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SGroomEditorViewport.h"

namespace UeAgentEditorViewport
{
	namespace
	{
		static constexpr const TCHAR* GroomCameraSpace = TEXT("groom_preview_world");

		static bool TryGetObjectField(const TSharedPtr<FJsonObject>& Obj, const FString& Key, TSharedPtr<FJsonObject>& OutObj)
		{
			const TSharedPtr<FJsonObject>* Found = nullptr;
			if (Obj.IsValid() && Obj->TryGetObjectField(Key, Found) && Found && Found->IsValid())
			{
				OutObj = *Found;
				return true;
			}
			return false;
		}

		static bool TryGetStringField(const TSharedPtr<FJsonObject>& Obj, const FString& Key, FString& OutValue)
		{
			return Obj.IsValid() && Obj->TryGetStringField(Key, OutValue);
		}

		static bool TryGetNumberField(const TSharedPtr<FJsonObject>& Obj, const FString& Key, double& OutValue)
		{
			return UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { Key }, OutValue);
		}

		static bool TryGetBoolField(const TSharedPtr<FJsonObject>& Obj, const FString& Key, bool& OutValue)
		{
			return Obj.IsValid() && Obj->TryGetBoolField(Key, OutValue);
		}

		static FString NormalizeToken(FString Value)
		{
			Value.TrimStartAndEndInline();
			Value.ToLowerInline();
			Value.ReplaceInline(TEXT("-"), TEXT("_"));
			return Value;
		}

		static bool TryReadBoolOption(const TSharedPtr<FJsonObject>& Options, const FString& Key, bool& OutValue)
		{
			if (TryGetBoolField(Options, Key, OutValue))
			{
				return true;
			}
			TSharedPtr<FJsonObject> Overlays;
			return TryGetObjectField(Options, TEXT("overlays"), Overlays) && TryGetBoolField(Overlays, Key, OutValue);
		}

		static bool TryReadVectorObject(const TSharedPtr<FJsonObject>& Obj, FVector& OutValue)
		{
			if (!Obj.IsValid())
			{
				return false;
			}
			double X = 0.0;
			double Y = 0.0;
			double Z = 0.0;
			if (!UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { TEXT("x"), TEXT("X") }, X)
				|| !UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { TEXT("y"), TEXT("Y") }, Y)
				|| !UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { TEXT("z"), TEXT("Z") }, Z))
			{
				return false;
			}
			OutValue = FVector(X, Y, Z);
			return true;
		}

		static bool TryReadVectorField(const TSharedPtr<FJsonObject>& Obj, const FString& Key, FVector& OutValue)
		{
			TSharedPtr<FJsonObject> VectorObj;
			if (TryGetObjectField(Obj, Key, VectorObj))
			{
				return TryReadVectorObject(VectorObj, OutValue);
			}
			const TSharedPtr<FJsonValue>* FieldValue = UeAgentJsonDiagnostics::FindFieldValue(Obj, Key);
			return FieldValue && UeAgentJsonDiagnostics::TryReadVectorValue(*FieldValue, OutValue);
		}

		static bool HasUserFields(const TSharedPtr<FJsonObject>& Obj)
		{
			return Obj.IsValid() && Obj->Values.Num() > 0;
		}

		static FString ToObjectPath(const FString& InPath)
		{
			FString Path = InPath;
			Path.TrimStartAndEndInline();
			if (Path.IsEmpty() || Path.Contains(TEXT(".")))
			{
				return Path;
			}
			const FString PackagePath = Path;
			const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
			return FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);
		}

		static FString ViewModeToString(const EViewModeIndex ViewMode)
		{
			switch (ViewMode)
			{
			case VMI_Lit: return TEXT("lit");
			case VMI_Unlit: return TEXT("unlit");
			case VMI_BrushWireframe: return TEXT("wireframe");
			case VMI_Wireframe: return TEXT("csg_wireframe");
			case VMI_Lit_Wireframe: return TEXT("lit_wireframe");
			case VMI_Lit_DetailLighting: return TEXT("detail_lighting");
			case VMI_LightingOnly: return TEXT("lighting_only");
			case VMI_ReflectionOverride: return TEXT("reflections");
			case VMI_VisualizeGroom: return TEXT("groom_visualization");
			default: return FString::Printf(TEXT("unknown_%d"), static_cast<int32>(ViewMode));
			}
		}

		static bool TryParseViewMode(const FString& InValue, EViewModeIndex& OutViewMode)
		{
			const FString Value = NormalizeToken(InValue);
			if (Value == TEXT("lit")) { OutViewMode = VMI_Lit; return true; }
			if (Value == TEXT("unlit")) { OutViewMode = VMI_Unlit; return true; }
			if (Value == TEXT("wireframe") || Value == TEXT("brush_wireframe")) { OutViewMode = VMI_BrushWireframe; return true; }
			if (Value == TEXT("csg_wireframe")) { OutViewMode = VMI_Wireframe; return true; }
			if (Value == TEXT("lit_wireframe")) { OutViewMode = VMI_Lit_Wireframe; return true; }
			if (Value == TEXT("detail_lighting")) { OutViewMode = VMI_Lit_DetailLighting; return true; }
			if (Value == TEXT("lighting_only")) { OutViewMode = VMI_LightingOnly; return true; }
			if (Value == TEXT("reflections") || Value == TEXT("reflection_override")) { OutViewMode = VMI_ReflectionOverride; return true; }
			if (Value == TEXT("groom_visualization") || Value == TEXT("visualize_groom") || Value == TEXT("groom")) { OutViewMode = VMI_VisualizeGroom; return true; }
			return false;
		}

		static TSharedPtr<FJsonObject> MakeBoundsJson(const FBox& Box)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetObjectField(TEXT("min"), MakeVectorJson(Box.Min));
			Obj->SetObjectField(TEXT("max"), MakeVectorJson(Box.Max));
			Obj->SetObjectField(TEXT("center"), MakeVectorJson(Box.GetCenter()));
			Obj->SetObjectField(TEXT("extent"), MakeVectorJson(Box.GetExtent()));
			return Obj;
		}

		static TSharedPtr<FJsonObject> MakeBoolOptionsJson(const TMap<FString, bool>& Values)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			for (const TPair<FString, bool>& Pair : Values)
			{
				Obj->SetBoolField(Pair.Key, Pair.Value);
			}
			return Obj;
		}

		static TSharedPtr<FJsonObject> MakePixelSignalJson(const TArray<FColor>& Pixels, const FIntPoint& Size)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetNumberField(TEXT("width"), Size.X);
			Obj->SetNumberField(TEXT("height"), Size.Y);
			Obj->SetNumberField(TEXT("sampled_pixel_count"), Pixels.Num());

			int32 BrightPixelCount = 0;
			int32 TransparentPixelCount = 0;
			TSet<uint32> UniqueColors;
			for (const FColor& Pixel : Pixels)
			{
				if ((static_cast<int32>(Pixel.R) + static_cast<int32>(Pixel.G) + static_cast<int32>(Pixel.B)) > 12)
				{
					++BrightPixelCount;
				}
				if (Pixel.A < 250)
				{
					++TransparentPixelCount;
				}
				if (UniqueColors.Num() <= 512)
				{
					UniqueColors.Add(Pixel.ToPackedARGB());
				}
			}
			Obj->SetNumberField(TEXT("bright_pixel_count"), BrightPixelCount);
			Obj->SetNumberField(TEXT("transparent_pixel_count"), TransparentPixelCount);
			Obj->SetNumberField(TEXT("unique_color_count_capped"), UniqueColors.Num());
			Obj->SetBoolField(TEXT("nonblank"), BrightPixelCount > 0 && UniqueColors.Num() > 1);
			return Obj;
		}

		static bool FindWidgetsByTypeRecursive(const TSharedRef<SWidget>& RootWidget, const FName& TypeName, TArray<TSharedRef<SWidget>>& OutWidgets)
		{
			bool bFound = false;
			if (RootWidget->GetType() == TypeName)
			{
				OutWidgets.Add(RootWidget);
				bFound = true;
			}
			FChildren* Children = RootWidget->GetChildren();
			if (!Children)
			{
				return bFound;
			}
			for (int32 ChildIndex = 0; ChildIndex < Children->Num(); ++ChildIndex)
			{
				bFound |= FindWidgetsByTypeRecursive(Children->GetChildAt(ChildIndex), TypeName, OutWidgets);
			}
			return bFound;
		}

		static TSharedPtr<SWidget> ChooseLargestWidget(const TArray<TSharedRef<SWidget>>& Widgets)
		{
			TSharedPtr<SWidget> Best;
			float BestArea = -1.0f;
			for (const TSharedRef<SWidget>& Widget : Widgets)
			{
				const FVector2D Size = Widget->GetCachedGeometry().GetLocalSize();
				const float Area = Size.X * Size.Y;
				if (!Best.IsValid() || Area > BestArea)
				{
					Best = Widget;
					BestArea = Area;
				}
			}
			return Best;
		}

		struct FResolvedScreenshotOptions
		{
			FString Format = TEXT("png");
			FString OutputPath;
			int32 Quality = 90;
			int32 MaxSize = 1600;
			int32 WarmupFrames = 2;
		};

		static bool ParseScreenshotOptions(const TSharedPtr<FJsonObject>& Params, FResolvedScreenshotOptions& OutOptions, FString& OutError)
		{
			FString Format = TEXT("png");
			TryGetStringField(Params, TEXT("format"), Format);
			Format = NormalizeToken(Format);
			if (Format == TEXT("jpeg"))
			{
				Format = TEXT("jpg");
			}
			if (Format != TEXT("png") && Format != TEXT("jpg") && Format != TEXT("webp"))
			{
				OutError = TEXT("unsupported_format");
				return false;
			}
			double QualityD = 90.0;
			if (TryGetNumberField(Params, TEXT("quality"), QualityD))
			{
				OutOptions.Quality = FMath::Clamp(static_cast<int32>(QualityD), 1, 100);
			}
			double MaxSizeD = 1600.0;
			if (TryGetNumberField(Params, TEXT("max_size"), MaxSizeD))
			{
				OutOptions.MaxSize = FMath::Clamp(static_cast<int32>(MaxSizeD), 64, 8192);
			}
			double WarmupD = 2.0;
			if (TryGetNumberField(Params, TEXT("warmup_frames"), WarmupD))
			{
				OutOptions.WarmupFrames = FMath::Clamp(static_cast<int32>(WarmupD), 0, 8);
			}

			FString OutputFile;
			if (TryGetStringField(Params, TEXT("output_file"), OutputFile) && !OutputFile.TrimStartAndEnd().IsEmpty())
			{
				OutOptions.OutputPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(OutputFile);
				FString Ext = FPaths::GetExtension(OutOptions.OutputPath).ToLower();
				if (Ext == TEXT("jpeg"))
				{
					Ext = TEXT("jpg");
				}
				if (Ext.IsEmpty())
				{
					OutOptions.OutputPath += FString::Printf(TEXT(".%s"), *Format);
				}
				else if (Ext != Format)
				{
					OutError = TEXT("output_file_extension_mismatch");
					return false;
				}
			}
			else
			{
				OutOptions.OutputPath = FUeAgentHttpServer::MakeShotPath(Format);
			}
			OutOptions.Format = Format;
			return true;
		}

		static bool CaptureEditorViewportClientBackbuffer(
			FEditorViewportClient* Client,
			const FString& AdapterName,
			const FResolvedScreenshotOptions& Options,
			TArray<FColor>& OutPixels,
			FIntPoint& OutSize,
			TSharedPtr<FJsonObject>& OutCaptureInfo,
			FString& OutError)
		{
			if (!Client || !Client->Viewport)
			{
				OutError = TEXT("invalid_editor_viewport_client");
				return false;
			}

			const FText OverrideSystem = NSLOCTEXT("UeAgentInterface", "GroomViewportScreenshotRealtimeOverrideSystem", "UeAgentInterfaceGroomViewportScreenshot");
			const bool bRealtimeBefore = Client->IsRealtime();
			const bool bNeedTemporaryRealtime = !Client->IsRealtime();
			if (bNeedTemporaryRealtime)
			{
				if (Client->HasRealtimeOverride(OverrideSystem))
				{
					Client->RemoveRealtimeOverride(OverrideSystem, false);
				}
				Client->AddRealtimeOverride(true, OverrideSystem);
			}

			FViewport* Viewport = Client->Viewport;
			int32 DrawsPerformed = 0;
			bool bAppliedFallbackViewportSize = false;
			const FIntPoint InitialViewportSize = Viewport->GetSizeXY();
			for (int32 FrameIndex = 0; FrameIndex < Options.WarmupFrames; ++FrameIndex)
			{
				Client->Invalidate();
				Viewport->Invalidate();
				Viewport->Draw(true);
				FlushRenderingCommands();
				++DrawsPerformed;
			}

			Client->Invalidate();
			Viewport->Invalidate();
			Viewport->Draw(true);
			FlushRenderingCommands();
			++DrawsPerformed;

			if (FApp::CanEverRender() && (Viewport->GetSizeXY().X <= 0 || Viewport->GetSizeXY().Y <= 0))
			{
				FSceneViewport* SceneViewport = static_cast<FSceneViewport*>(Viewport);
				const uint32 FallbackSize = static_cast<uint32>(FMath::Clamp(Options.MaxSize, 512, 2048));
				SceneViewport->SetFixedViewportSize(FallbackSize, FallbackSize);
				Client->Invalidate();
				Viewport->Invalidate();
				Viewport->Draw(true);
				FlushRenderingCommands();
				++DrawsPerformed;
				bAppliedFallbackViewportSize = Viewport->GetSizeXY().X > 0 && Viewport->GetSizeXY().Y > 0;
			}

			if (bNeedTemporaryRealtime && Client->HasRealtimeOverride(OverrideSystem))
			{
				Client->RemoveRealtimeOverride(OverrideSystem, false);
			}

			const FIntPoint Size = Viewport->GetSizeXY();
			if (Size.X <= 0 || Size.Y <= 0)
			{
				if (bAppliedFallbackViewportSize)
				{
					static_cast<FSceneViewport*>(Viewport)->SetFixedViewportSize(0, 0);
				}
				OutError = TEXT("invalid_viewport_size");
				return false;
			}

			TArray<FColor> Pixels;
			Pixels.AddUninitialized(Size.X * Size.Y);
			if (!Viewport->ReadPixels(Pixels))
			{
				if (bAppliedFallbackViewportSize)
				{
					static_cast<FSceneViewport*>(Viewport)->SetFixedViewportSize(0, 0);
				}
				OutError = TEXT("read_pixels_failed");
				return false;
			}

			if (bAppliedFallbackViewportSize)
			{
				static_cast<FSceneViewport*>(Viewport)->SetFixedViewportSize(0, 0);
			}

			OutPixels = MoveTemp(Pixels);
			OutSize = Size;
			OutCaptureInfo = MakeShared<FJsonObject>();
			OutCaptureInfo->SetStringField(TEXT("schema"), TEXT("uai.viewport_capture.v1"));
			OutCaptureInfo->SetStringField(TEXT("render_path"), TEXT("editor_viewport_client_backbuffer"));
			OutCaptureInfo->SetStringField(TEXT("adapter"), AdapterName);
			OutCaptureInfo->SetBoolField(TEXT("temporary_realtime_override"), bNeedTemporaryRealtime);
			OutCaptureInfo->SetBoolField(TEXT("realtime_before_capture"), bRealtimeBefore);
			OutCaptureInfo->SetNumberField(TEXT("draws_performed"), DrawsPerformed);
			OutCaptureInfo->SetBoolField(TEXT("fallback_viewport_size_applied"), bAppliedFallbackViewportSize);
			TSharedPtr<FJsonObject> InitialSizeJson = MakeShared<FJsonObject>();
			InitialSizeJson->SetNumberField(TEXT("x"), InitialViewportSize.X);
			InitialSizeJson->SetNumberField(TEXT("y"), InitialViewportSize.Y);
			OutCaptureInfo->SetObjectField(TEXT("initial_viewport_size"), InitialSizeJson);
			OutCaptureInfo->SetStringField(TEXT("view_mode_name"), ViewModeToString(Client->GetViewMode()));
			OutCaptureInfo->SetNumberField(TEXT("view_mode_value"), static_cast<int32>(Client->GetViewMode()));
			return true;
		}

		static FName ResolveGroomVisualizationModeName(const FString& Input)
		{
			const FString Value = NormalizeToken(Input);
			if (Value.IsEmpty() || Value == TEXT("none") || Value == TEXT("lit"))
			{
				return NAME_None;
			}
			if (Value == TEXT("guides") || Value == TEXT("guide") || Value == TEXT("sim_guides") || Value == TEXT("sim_hair_strands"))
			{
				return FName(GetGroomViewModeName(EGroomViewMode::SimHairStrands));
			}
			if (Value == TEXT("strands") || Value == TEXT("render_strands") || Value == TEXT("render_hair_strands"))
			{
				return FName(GetGroomViewModeName(EGroomViewMode::RenderHairStrands));
			}
			if (Value == TEXT("cards") || Value == TEXT("card_guides"))
			{
				return FName(GetGroomViewModeName(EGroomViewMode::CardGuides));
			}
			if (Value == TEXT("control_points") || Value == TEXT("points"))
			{
				return FName(GetGroomViewModeName(EGroomViewMode::ControlPoints));
			}
			if (Value == TEXT("cluster_aabb") || Value == TEXT("aabb"))
			{
				return FName(GetGroomViewModeName(EGroomViewMode::ClusterAABB));
			}
			const FGroomVisualizationData& Data = GetGroomVisualizationData();
			for (FGroomVisualizationData::TModeMap::TConstIterator It = Data.GetModeMap().CreateConstIterator(); It; ++It)
			{
				const FGroomVisualizationData::FModeRecord& Record = It.Value();
				if (NormalizeToken(Record.ModeName.ToString()) == Value || NormalizeToken(Record.ModeString) == Value)
				{
					return Record.ModeName;
				}
			}
			return FName(*Input);
		}

		static bool IsGroomAssetType(const FString& AssetType)
		{
			const FString Type = NormalizeToken(AssetType);
			return Type.IsEmpty()
				|| Type == TEXT("groom")
				|| Type == TEXT("groom_asset")
				|| Type == TEXT("ugroomasset");
		}

		static bool IsGroomBindingType(const FString& AssetType)
		{
			const FString Type = NormalizeToken(AssetType);
			return Type == TEXT("groom_binding")
				|| Type == TEXT("groom_binding_asset")
				|| Type == TEXT("ugroombindingasset");
		}

		static UObject* LoadTargetAsset(const FString& AssetPath)
		{
			if (AssetPath.TrimStartAndEnd().IsEmpty())
			{
				return nullptr;
			}
			if (UObject* Asset = LoadObject<UGroomAsset>(nullptr, *ToObjectPath(AssetPath)))
			{
				return Asset;
			}
			return LoadObject<UGroomBindingAsset>(nullptr, *ToObjectPath(AssetPath));
		}

		static IAssetEditorInstance* GetGroomAssetEditor(UGroomAsset* Asset, const bool bOpenIfNeeded, FString& OutError)
		{
			if (!Asset)
			{
				OutError = TEXT("groom_asset_not_found");
				return nullptr;
			}
			if (!GEditor)
			{
				OutError = TEXT("editor_unavailable");
				return nullptr;
			}
			UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
			if (!AssetEditorSubsystem)
			{
				OutError = TEXT("asset_editor_subsystem_unavailable");
				return nullptr;
			}
			if (bOpenIfNeeded)
			{
				AssetEditorSubsystem->OpenEditorForAsset(Asset);
				if (FSlateApplication::IsInitialized())
				{
					FSlateApplication::Get().Tick();
				}
			}
			IAssetEditorInstance* AssetEditor = AssetEditorSubsystem->FindEditorForAsset(Asset, false);
			if (!AssetEditor)
			{
				OutError = bOpenIfNeeded ? TEXT("groom_editor_open_failed") : TEXT("groom_editor_not_opened");
				return nullptr;
			}
			return AssetEditor;
		}

		static TSharedPtr<SWindow> ResolveGroomEditorWindow(IAssetEditorInstance* AssetEditor, FString& OutError)
		{
			if (!AssetEditor)
			{
				OutError = TEXT("groom_editor_not_opened");
				return nullptr;
			}
			const TSharedPtr<FTabManager> TabManager = AssetEditor->GetAssociatedTabManager();
			if (!TabManager.IsValid())
			{
				OutError = TEXT("groom_tab_manager_not_found");
				return nullptr;
			}
			const FName ViewportTabId(TEXT("GroomCustomAssetEditor_Render"));
			TSharedPtr<SDockTab> ViewportTab = TabManager->FindExistingLiveTab(FTabId(ViewportTabId));
			if (!ViewportTab.IsValid())
			{
				ViewportTab = TabManager->TryInvokeTab(FTabId(ViewportTabId), false);
			}
			if (ViewportTab.IsValid())
			{
				ViewportTab->ActivateInParent(ETabActivationCause::SetDirectly);
				ViewportTab->SlatePrepass();
			}
			if (FSlateApplication::IsInitialized())
			{
				FSlateApplication::Get().Tick();
			}
			const TSharedPtr<SDockTab> OwnerTab = TabManager->GetOwnerTab();
			if (!OwnerTab.IsValid())
			{
				OutError = TEXT("groom_owner_tab_not_found");
				return nullptr;
			}
			OwnerTab->ActivateInParent(ETabActivationCause::SetDirectly);
			OwnerTab->SlatePrepass();
			if (FSlateApplication::IsInitialized())
			{
				FSlateApplication::Get().Tick();
			}
			TSharedPtr<SWindow> Window = OwnerTab->GetParentWindow();
			if (!Window.IsValid())
			{
				OutError = TEXT("groom_window_not_found");
			}
			return Window;
		}

		static TSharedPtr<SGroomEditorViewport> ResolveGroomViewportWidget(UGroomAsset* Asset, const bool bOpenIfNeeded, FString& OutError)
		{
			IAssetEditorInstance* AssetEditor = GetGroomAssetEditor(Asset, bOpenIfNeeded, OutError);
			if (!AssetEditor)
			{
				return nullptr;
			}
			TSharedPtr<SWindow> Window = ResolveGroomEditorWindow(AssetEditor, OutError);
			if (!Window.IsValid())
			{
				return nullptr;
			}
			TArray<TSharedRef<SWidget>> CandidateWidgets;
			FindWidgetsByTypeRecursive(Window.ToSharedRef(), FName(TEXT("SGroomEditorViewport")), CandidateWidgets);
			TSharedPtr<SWidget> Widget = ChooseLargestWidget(CandidateWidgets);
			if (!Widget.IsValid())
			{
				OutError = TEXT("groom_editor_viewport_not_found");
				return nullptr;
			}
			TSharedPtr<SGroomEditorViewport> ViewportWidget = StaticCastSharedPtr<SGroomEditorViewport>(Widget);
			if (!ViewportWidget.IsValid() || !ViewportWidget->GetViewportClient().IsValid())
			{
				OutError = TEXT("groom_editor_viewport_invalid");
				return nullptr;
			}
			return ViewportWidget;
		}

		struct FGroomRuntime
		{
			UGroomAsset* Asset = nullptr;
			TSharedPtr<SGroomEditorViewport> ViewportWidget;
			FEditorViewportClient* Client = nullptr;
		};

		static bool ResolveGroomRuntime(UGroomAsset* Asset, const bool bOpenIfNeeded, FGroomRuntime& OutRuntime, FString& OutError)
		{
			OutRuntime = FGroomRuntime();
			OutRuntime.Asset = Asset;
			OutRuntime.ViewportWidget = ResolveGroomViewportWidget(Asset, bOpenIfNeeded, OutError);
			if (!OutRuntime.ViewportWidget.IsValid())
			{
				return false;
			}
			const TSharedPtr<FEditorViewportClient> Client = OutRuntime.ViewportWidget->GetViewportClient();
			if (!Client.IsValid())
			{
				OutError = TEXT("groom_viewport_client_not_found");
				return false;
			}
			OutRuntime.Client = Client.Get();
			return true;
		}

		static FBox ComputeGroomBounds(UGroomAsset* Asset, bool& bUsedFallback)
		{
			bUsedFallback = true;
			FBox Box(ForceInit);
			if (Asset)
			{
				UGroomComponent* Component = NewObject<UGroomComponent>(GetTransientPackage(), NAME_None, RF_Transient);
				if (Component)
				{
#if WITH_EDITORONLY_DATA
					Component->SetPreviewMode(true);
#endif
					Component->SetGroomAsset(Asset);
					Component->UpdateBounds();
					Box = Component->Bounds.GetBox();
				}
			}
			if (Box.IsValid && Box.GetExtent().Size() > KINDA_SMALL_NUMBER)
			{
				bUsedFallback = false;
				return Box;
			}
			return FBox::BuildAABB(FVector::ZeroVector, FVector(50.0));
		}

		static TSharedPtr<FJsonObject> MakeGroomVisualOptionsJson(const FGroomRuntime& Runtime)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			FEditorViewportClient* Client = Runtime.Client;
			Obj->SetStringField(TEXT("view_mode"), Client ? ViewModeToString(Client->GetViewMode()) : TEXT("unknown"));
			Obj->SetNumberField(TEXT("view_mode_value"), Client ? static_cast<int32>(Client->GetViewMode()) : -1);
			TMap<FString, bool> Overlays;
			Overlays.Add(TEXT("grid"), Client ? Client->EngineShowFlags.Grid != 0 : false);
			Overlays.Add(TEXT("bounds"), Client ? Client->EngineShowFlags.Bounds != 0 : false);
			Obj->SetObjectField(TEXT("overlays"), MakeBoolOptionsJson(Overlays));
			Obj->SetBoolField(TEXT("realtime"), Client ? Client->IsRealtime() : false);
			Obj->SetStringField(TEXT("groom_visualization"), Client ? Client->CurrentGroomVisualizationMode.ToString() : FString());
			Obj->SetBoolField(TEXT("groom_visualization_enabled"), Client && Client->GetViewMode() == VMI_VisualizeGroom);
			return Obj;
		}

		static TSharedPtr<FJsonObject> MakeGroomPreviewStateJson(const FGroomRuntime& Runtime)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("context"), TEXT("groom"));
			Obj->SetStringField(TEXT("asset_path"), Runtime.Asset ? Runtime.Asset->GetPathName() : FString());
			Obj->SetNumberField(TEXT("hair_group_count"), Runtime.Asset ? Runtime.Asset->GetNumHairGroups() : 0);
			Obj->SetNumberField(TEXT("info_group_count"), Runtime.Asset ? Runtime.Asset->GetHairGroupsInfo().Num() : 0);
			Obj->SetNumberField(TEXT("current_lod"), Runtime.ViewportWidget.IsValid() ? Runtime.ViewportWidget->GetCurrentLOD() : -1);
			Obj->SetNumberField(TEXT("forced_lod"), Runtime.ViewportWidget.IsValid() ? Runtime.ViewportWidget->GetCurrentLOD() : -1);
			Obj->SetNumberField(TEXT("lod_count"), Runtime.ViewportWidget.IsValid() ? Runtime.ViewportWidget->GetLODCount() : -1);
			bool bFallbackBounds = true;
			const FBox Bounds = ComputeGroomBounds(Runtime.Asset, bFallbackBounds);
			Obj->SetObjectField(TEXT("bounds"), MakeBoundsJson(Bounds));
			Obj->SetStringField(TEXT("bounds_space"), GroomCameraSpace);
			Obj->SetStringField(TEXT("bounds_source"), bFallbackBounds ? TEXT("fallback_default_bounds") : TEXT("transient_groom_component"));
			return Obj;
		}

		class FGroomViewportAdapter final : public IViewportAdapter
		{
		public:
			virtual FString GetName() const override
			{
				return TEXT("groom");
			}

			virtual bool CanResolve(const FResolveRequest& Request) const override
			{
				if (NormalizeToken(Request.Target.Scope) != TEXT("asset_editor"))
				{
					return false;
				}
				if (IsGroomAssetType(Request.Target.AssetType) || IsGroomBindingType(Request.Target.AssetType))
				{
					return true;
				}
				if (Request.Target.AssetType.TrimStartAndEnd().IsEmpty())
				{
					return LoadObject<UGroomAsset>(nullptr, *ToObjectPath(Request.Target.AssetPath)) != nullptr
						|| LoadObject<UGroomBindingAsset>(nullptr, *ToObjectPath(Request.Target.AssetPath)) != nullptr;
				}
				return false;
			}

			virtual bool Resolve(const FResolveRequest& Request, FResolvedViewport& OutViewport, FString& OutError) const override
			{
				UObject* Asset = LoadTargetAsset(Request.Target.AssetPath);
				if (!Asset)
				{
					OutError = TEXT("groom_asset_not_found");
					return false;
				}
				if (UGroomBindingAsset* BindingAsset = Cast<UGroomBindingAsset>(Asset))
				{
					OutError = BindingAsset->GetGroom()
						? TEXT("groom_binding_preview_requires_groom_asset_target_and_binding_index")
						: TEXT("groom_binding_missing_groom_asset");
					return false;
				}
				UGroomAsset* GroomAsset = Cast<UGroomAsset>(Asset);
				if (!GroomAsset)
				{
					OutError = TEXT("unsupported_groom_asset_type");
					return false;
				}
				FGroomRuntime Runtime;
				if (!ResolveGroomRuntime(GroomAsset, Request.Target.bOpenEditorIfNeeded, Runtime, OutError))
				{
					return false;
				}
				OutViewport = FResolvedViewport();
				OutViewport.Target = Request.Target;
				OutViewport.Target.AssetPath = GroomAsset->GetPathName();
				OutViewport.Target.AssetType = TEXT("groom");
				OutViewport.Target.ViewportRole = Request.Target.ViewportRole.IsEmpty() ? TEXT("primary") : Request.Target.ViewportRole;
				OutViewport.AdapterName = GetName();
				OutViewport.ClientClass = TEXT("FGroomEditorViewportClient");
				OutViewport.ViewportClient = Runtime.Client;
				OutViewport.Asset = GroomAsset;
				return true;
			}

			virtual bool ReadState(const FResolvedViewport& Viewport, FViewportState& OutState, FString& OutError) const override
			{
				FGroomRuntime Runtime;
				if (!ResolveGroomRuntime(Cast<UGroomAsset>(Viewport.Asset), false, Runtime, OutError))
				{
					return false;
				}
				OutState = FViewportState();
				OutState.Target = Viewport.Target;
				OutState.Target.AssetPath = Runtime.Asset ? Runtime.Asset->GetPathName() : Viewport.Target.AssetPath;
				OutState.Target.AssetType = TEXT("groom");
				OutState.Adapter = GetName();
				OutState.ClientClass = Viewport.ClientClass;
				OutState.Camera.Location = Runtime.Client->GetViewLocation();
				OutState.Camera.Rotation = Runtime.Client->GetViewRotation();
				OutState.Camera.Fov = Runtime.Client->ViewFOV;
				OutState.Camera.Projection = TEXT("perspective");
				OutState.Camera.Space = GroomCameraSpace;
				OutState.Camera.bValid = true;
				OutState.Capabilities.bCamera = true;
				OutState.Capabilities.bVisualOptions = true;
				OutState.Capabilities.bPreviewState = true;
				OutState.Capabilities.bScreenshot = true;
				OutState.Capabilities.FocusModes = { TEXT("groom_bounds"), TEXT("asset_bounds"), TEXT("bounds"), TEXT("selection"), TEXT("custom_bounds"), TEXT("custom_point") };
				OutState.Capabilities.ScreenshotBackends = { TEXT("groom_editor_backbuffer") };
				OutState.Capabilities.VisualOptionKeys = { TEXT("view_mode"), TEXT("grid"), TEXT("bounds"), TEXT("realtime"), TEXT("groom_visualization"), TEXT("guides"), TEXT("strands"), TEXT("cards"), TEXT("control_points"), TEXT("cluster_aabb") };
				OutState.Capabilities.PreviewStateKeys = { TEXT("lod"), TEXT("forced_lod"), TEXT("reset_lod") };
				OutState.Capabilities.UnsupportedOptions = { TEXT("groom_binding_asset_direct_target") };
				OutState.VisualOptions = MakeGroomVisualOptionsJson(Runtime);
				OutState.PreviewState = MakeGroomPreviewStateJson(Runtime);
				return true;
			}

			virtual bool WriteCamera(const FResolvedViewport& Viewport, const FViewportCamera& Camera, FString& OutError) const override
			{
				FEditorViewportClient* Client = Viewport.ViewportClient;
				if (!Client)
				{
					FGroomRuntime Runtime;
					if (!ResolveGroomRuntime(Cast<UGroomAsset>(Viewport.Asset), false, Runtime, OutError))
					{
						return false;
					}
					Client = Runtime.Client;
				}
				if (!Client)
				{
					OutError = TEXT("invalid_groom_viewport");
					return false;
				}
				Client->SetViewLocation(Camera.Location);
				Client->SetViewRotation(Camera.Rotation);
				Client->ViewFOV = Camera.Fov;
				Client->Invalidate();
				if (Client->Viewport)
				{
					Client->Viewport->Invalidate();
				}
				return true;
			}

			virtual bool Focus(const FResolvedViewport& Viewport, const TSharedPtr<FJsonObject>& FocusObj, TSharedPtr<FJsonObject>& OutData, FString& OutError) const override
			{
				FGroomRuntime Runtime;
				if (!ResolveGroomRuntime(Cast<UGroomAsset>(Viewport.Asset), false, Runtime, OutError))
				{
					return false;
				}
				FString Mode = TEXT("groom_bounds");
				TryGetStringField(FocusObj, TEXT("mode"), Mode);
				Mode = NormalizeToken(Mode);

				FBox FocusBox(ForceInit);
				if (Mode == TEXT("groom_bounds") || Mode == TEXT("asset_bounds") || Mode == TEXT("bounds"))
				{
					bool bFallback = true;
					FocusBox = ComputeGroomBounds(Runtime.Asset, bFallback);
				}
				else if (Mode == TEXT("selection"))
				{
					bool bFallback = true;
					FocusBox = ComputeGroomBounds(Runtime.Asset, bFallback);
				}
				else if (Mode == TEXT("custom_bounds"))
				{
					FVector Min = FVector::ZeroVector;
					FVector Max = FVector::ZeroVector;
					if (!TryReadVectorField(FocusObj, TEXT("min"), Min) || !TryReadVectorField(FocusObj, TEXT("max"), Max))
					{
						TSharedPtr<FJsonObject> BoundsObj;
						if (!TryGetObjectField(FocusObj, TEXT("bounds"), BoundsObj)
							|| !TryReadVectorField(BoundsObj, TEXT("min"), Min)
							|| !TryReadVectorField(BoundsObj, TEXT("max"), Max))
						{
							OutError = TEXT("missing_custom_bounds");
							return false;
						}
					}
					FocusBox = FBox(Min, Max);
				}
				else if (Mode == TEXT("custom_point"))
				{
					FVector Point = FVector::ZeroVector;
					if (!TryReadVectorField(FocusObj, TEXT("point"), Point))
					{
						OutError = TEXT("missing_custom_point");
						return false;
					}
					double Radius = 50.0;
					TryGetNumberField(FocusObj, TEXT("radius"), Radius);
					FocusBox = FBox::BuildAABB(Point, FVector(FMath::Max(static_cast<float>(Radius), 1.0f)));
				}
				else
				{
					OutError = TEXT("unsupported_focus_mode");
					return false;
				}

				if (!FocusBox.IsValid)
				{
					OutError = TEXT("invalid_focus_bounds");
					return false;
				}
				Runtime.Client->FocusViewportOnBox(FocusBox);
				Runtime.Client->SetLookAtLocation(FocusBox.GetCenter(), false);
				Runtime.Client->Invalidate();
				if (Runtime.Client->Viewport)
				{
					Runtime.Client->Viewport->Invalidate();
				}
				OutData = MakeShared<FJsonObject>();
				OutData->SetStringField(TEXT("mode"), Mode);
				OutData->SetStringField(TEXT("bounds_space"), GroomCameraSpace);
				OutData->SetObjectField(TEXT("bounds"), MakeBoundsJson(FocusBox));
				return true;
			}

			virtual bool ApplyVisualOptions(const FResolvedViewport& Viewport, const TSharedPtr<FJsonObject>& Options, TSharedPtr<FJsonObject>& OutData, FString& OutError) const override
			{
				FGroomRuntime Runtime;
				if (!ResolveGroomRuntime(Cast<UGroomAsset>(Viewport.Asset), false, Runtime, OutError))
				{
					return false;
				}
				FString ViewModeText;
				if (TryGetStringField(Options, TEXT("view_mode"), ViewModeText))
				{
					EViewModeIndex ViewMode;
					if (!TryParseViewMode(ViewModeText, ViewMode))
					{
						OutError = TEXT("unsupported_view_mode");
						return false;
					}
					Runtime.Client->SetViewMode(ViewMode);
				}
				bool bValue = false;
				if (TryReadBoolOption(Options, TEXT("grid"), bValue))
				{
					Runtime.Client->EngineShowFlags.SetGrid(bValue);
				}
				if (TryReadBoolOption(Options, TEXT("bounds"), bValue))
				{
					Runtime.Client->EngineShowFlags.SetBounds(bValue);
				}
				if (TryReadBoolOption(Options, TEXT("realtime"), bValue))
				{
					Runtime.Client->SetRealtime(bValue);
				}

				FString GroomVisualization;
				if (TryGetStringField(Options, TEXT("groom_visualization"), GroomVisualization)
					|| TryGetStringField(Options, TEXT("visualization_mode"), GroomVisualization))
				{
					const FName ModeName = ResolveGroomVisualizationModeName(GroomVisualization);
					if (ModeName.IsNone())
					{
						Runtime.Client->SetViewMode(VMI_Lit);
					}
					else
					{
						Runtime.Client->SetViewMode(VMI_VisualizeGroom);
						Runtime.Client->ChangeGroomVisualizationMode(ModeName);
					}
				}
				else if (TryReadBoolOption(Options, TEXT("guides"), bValue) && bValue)
				{
					Runtime.Client->SetViewMode(VMI_VisualizeGroom);
					Runtime.Client->ChangeGroomVisualizationMode(ResolveGroomVisualizationModeName(TEXT("guides")));
				}
				else if (TryReadBoolOption(Options, TEXT("strands"), bValue) && bValue)
				{
					Runtime.Client->SetViewMode(VMI_VisualizeGroom);
					Runtime.Client->ChangeGroomVisualizationMode(ResolveGroomVisualizationModeName(TEXT("strands")));
				}
				else if (TryReadBoolOption(Options, TEXT("cards"), bValue) && bValue)
				{
					Runtime.Client->SetViewMode(VMI_VisualizeGroom);
					Runtime.Client->ChangeGroomVisualizationMode(ResolveGroomVisualizationModeName(TEXT("cards")));
				}
				else if (TryReadBoolOption(Options, TEXT("control_points"), bValue) && bValue)
				{
					Runtime.Client->SetViewMode(VMI_VisualizeGroom);
					Runtime.Client->ChangeGroomVisualizationMode(ResolveGroomVisualizationModeName(TEXT("control_points")));
				}
				else if (TryReadBoolOption(Options, TEXT("cluster_aabb"), bValue) && bValue)
				{
					Runtime.Client->SetViewMode(VMI_VisualizeGroom);
					Runtime.Client->ChangeGroomVisualizationMode(ResolveGroomVisualizationModeName(TEXT("cluster_aabb")));
				}

				Runtime.Client->Invalidate();
				if (Runtime.Client->Viewport)
				{
					Runtime.Client->Viewport->Invalidate();
				}
				OutData = MakeGroomVisualOptionsJson(Runtime);
				return true;
			}

			virtual bool ApplyPreviewState(const FResolvedViewport& Viewport, const TSharedPtr<FJsonObject>& PreviewState, TSharedPtr<FJsonObject>& OutData, FString& OutError) const override
			{
				FGroomRuntime Runtime;
				if (!ResolveGroomRuntime(Cast<UGroomAsset>(Viewport.Asset), false, Runtime, OutError))
				{
					return false;
				}
				if (!HasUserFields(PreviewState))
				{
					OutData = MakeShared<FJsonObject>();
					OutData->SetBoolField(TEXT("applied"), false);
					OutData->SetStringField(TEXT("reason"), TEXT("empty_preview_state"));
					return true;
				}
				bool bResetLod = false;
				if (TryGetBoolField(PreviewState, TEXT("reset_lod"), bResetLod) && bResetLod && Runtime.ViewportWidget.IsValid())
				{
					Runtime.ViewportWidget->SetLODLevel(-1);
				}
				double Lod = 0.0;
				if ((TryGetNumberField(PreviewState, TEXT("lod"), Lod) || TryGetNumberField(PreviewState, TEXT("forced_lod"), Lod)) && Runtime.ViewportWidget.IsValid())
				{
					Runtime.ViewportWidget->SetLODLevel(static_cast<int32>(Lod));
				}
				OutData = MakeGroomPreviewStateJson(Runtime);
				return true;
			}

			virtual bool CaptureScreenshot(const FResolvedViewport& Viewport, const TSharedPtr<FJsonObject>& Params, FViewportScreenshotResult& OutResult, FString& OutError) const override
			{
				if (!Viewport.ViewportClient)
				{
					OutError = TEXT("invalid_groom_viewport");
					return false;
				}
				const UUeAgentInterfaceSettings* Settings = UUeAgentInterfaceSettings::Get();
				if (!Settings || !Settings->bEnableScreenshots)
				{
					OutError = TEXT("screenshots_disabled");
					return false;
				}
				FResolvedScreenshotOptions Options;
				if (!ParseScreenshotOptions(Params, Options, OutError))
				{
					return false;
				}
				TArray<FColor> Pixels;
				FIntPoint Size(0, 0);
				TSharedPtr<FJsonObject> CaptureInfo;
				if (!CaptureEditorViewportClientBackbuffer(Viewport.ViewportClient, GetName(), Options, Pixels, Size, CaptureInfo, OutError))
				{
					return false;
				}
				TArray<FColor> Resized;
				FIntPoint ResizedSize(0, 0);
				if (!FUeAgentHttpServer::ResizePixelsMaxSize(Pixels, Size, Options.MaxSize, Resized, ResizedSize, OutError))
				{
					return false;
				}
				if (!FUeAgentHttpServer::WriteCompressedImage(Resized, ResizedSize, Options.Format, Options.Quality, Options.OutputPath, OutError))
				{
					return false;
				}
				OutResult.FilePath = Options.OutputPath;
				OutResult.Format = Options.Format;
				OutResult.Width = ResizedSize.X;
				OutResult.Height = ResizedSize.Y;
				OutResult.Bytes = IFileManager::Get().FileSize(*Options.OutputPath);
				OutResult.CaptureBackend = TEXT("groom_editor_backbuffer");
				OutResult.PixelSignal = MakePixelSignalJson(Resized, ResizedSize);
				OutResult.Capture = CaptureInfo;
				return true;
			}

			virtual bool SnapshotState(const FResolvedViewport& Viewport, FViewportSnapshot& OutSnapshot, FString& OutError) const override
			{
				OutSnapshot = FViewportSnapshot();
				if (!ReadState(Viewport, OutSnapshot.State, OutError))
				{
					return false;
				}
				OutSnapshot.bValid = true;
				return true;
			}

			virtual bool RestoreState(const FResolvedViewport& Viewport, const FViewportSnapshot& Snapshot, FString& OutError) const override
			{
				if (!Snapshot.bValid)
				{
					OutError = TEXT("invalid_viewport_snapshot");
					return false;
				}
				if (!WriteCamera(Viewport, Snapshot.State.Camera, OutError))
				{
					return false;
				}
				TSharedPtr<FJsonObject> AppliedVisual;
				if (!ApplyVisualOptions(Viewport, Snapshot.State.VisualOptions, AppliedVisual, OutError))
				{
					return false;
				}
				TSharedPtr<FJsonObject> AppliedPreview;
				if (Snapshot.State.PreviewState.IsValid())
				{
					ApplyPreviewState(Viewport, Snapshot.State.PreviewState, AppliedPreview, OutError);
				}
				return true;
			}
		};
	}

	TSharedRef<IViewportAdapter> MakeGroomViewportAdapter()
	{
		return MakeShared<FGroomViewportAdapter>();
	}
}
