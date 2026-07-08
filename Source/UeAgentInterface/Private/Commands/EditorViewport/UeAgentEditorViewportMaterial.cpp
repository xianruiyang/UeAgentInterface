// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commands/EditorViewport/UeAgentEditorViewportMaterial.h"

#include "Utilities/UeAgentJsonDiagnostics.h"
#include "UeAgentInterfaceSettings.h"
#include "Server/UeAgentHttpServer.h"

#include "AdvancedPreviewScene.h"
#include "Components/MeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Editor/UnrealEdEngine.h"
#include "Engine/StaticMesh.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Commands/UICommandList.h"
#include "HAL/FileManager.h"
#include "MaterialEditorActions.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Materials/MaterialInstance.h"
#include "Misc/App.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "RenderingThread.h"
#include "SEditorViewport.h"
#include "ShowFlags.h"
#include "Slate/SceneViewport.h"
#include "SMaterialEditorViewport.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "ThumbnailRendering/ThumbnailManager.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "UObject/UObjectIterator.h"
#include "UnrealEdGlobals.h"
#include "Widgets/Docking/SDockTab.h"

namespace UeAgentEditorViewport
{
	namespace
	{
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

		static bool HasUserFields(const TSharedPtr<FJsonObject>& Obj)
		{
			return Obj.IsValid() && Obj->Values.Num() > 0;
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
			case VMI_CollisionPawn: return TEXT("player_collision");
			case VMI_CollisionVisibility: return TEXT("visibility_collision");
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
			if (Value == TEXT("player_collision") || Value == TEXT("pawn_collision")) { OutViewMode = VMI_CollisionPawn; return true; }
			if (Value == TEXT("visibility_collision")) { OutViewMode = VMI_CollisionVisibility; return true; }
			return false;
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

			const FText OverrideSystem = NSLOCTEXT("UeAgentInterface", "MaterialViewportScreenshotRealtimeOverrideSystem", "UeAgentInterfaceMaterialViewportScreenshot");
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

		static bool TryReadCommandBool(const TSharedPtr<FUICommandList>& CommandList, const TSharedPtr<FUICommandInfo>& Command, bool& OutValue)
		{
			if (!CommandList.IsValid() || !Command.IsValid() || !CommandList->IsActionMapped(Command))
			{
				return false;
			}
			OutValue = CommandList->GetCheckState(Command.ToSharedRef()) == ECheckBoxState::Checked;
			return true;
		}

		static bool TrySetCommandBool(const TSharedPtr<FUICommandList>& CommandList, const TSharedPtr<FUICommandInfo>& Command, const bool bValue)
		{
			if (!CommandList.IsValid() || !Command.IsValid() || !CommandList->IsActionMapped(Command))
			{
				return false;
			}
			const bool bCurrent = CommandList->GetCheckState(Command.ToSharedRef()) == ECheckBoxState::Checked;
			if (bCurrent != bValue)
			{
				return CommandList->TryExecuteAction(Command.ToSharedRef());
			}
			return true;
		}

		static FString PreviewPrimitiveToString(const EThumbnailPrimType PrimType)
		{
			switch (PrimType)
			{
			case TPT_Cylinder: return TEXT("cylinder");
			case TPT_Sphere: return TEXT("sphere");
			case TPT_Plane: return TEXT("plane");
			case TPT_Cube: return TEXT("cube");
			case TPT_None: return TEXT("custom_mesh");
			default: return FString::Printf(TEXT("unknown_%d"), static_cast<int32>(PrimType));
			}
		}

		static bool TryParsePreviewPrimitive(const FString& InValue, EThumbnailPrimType& OutPrimType)
		{
			const FString Value = NormalizeToken(InValue);
			if (Value == TEXT("cylinder")) { OutPrimType = TPT_Cylinder; return true; }
			if (Value == TEXT("sphere")) { OutPrimType = TPT_Sphere; return true; }
			if (Value == TEXT("plane")) { OutPrimType = TPT_Plane; return true; }
			if (Value == TEXT("cube")) { OutPrimType = TPT_Cube; return true; }
			return false;
		}

		static UStaticMesh* GetThumbnailPrimitiveMesh(const EThumbnailPrimType PrimType)
		{
			if (!GUnrealEd || !GUnrealEd->GetThumbnailManager())
			{
				return nullptr;
			}
			switch (PrimType)
			{
			case TPT_Cylinder: return GUnrealEd->GetThumbnailManager()->EditorCylinder;
			case TPT_Sphere: return GUnrealEd->GetThumbnailManager()->EditorSphere;
			case TPT_Plane: return GUnrealEd->GetThumbnailManager()->EditorPlane;
			case TPT_Cube: return GUnrealEd->GetThumbnailManager()->EditorCube;
			default: return nullptr;
			}
		}

		static bool SetPreviewPrimitiveTransient(SMaterialEditor3DPreviewViewport& ViewportWidget, const EThumbnailPrimType PrimType, FString& OutError)
		{
			UStaticMesh* PrimitiveMesh = GetThumbnailPrimitiveMesh(PrimType);
			if (!PrimitiveMesh)
			{
				OutError = TEXT("material_preview_primitive_mesh_not_found");
				return false;
			}

			const TSharedRef<FAdvancedPreviewScene> PreviewScene = ViewportWidget.GetPreviewScene();
			if (ViewportWidget.PreviewMeshComponent)
			{
				PreviewScene->RemoveComponent(ViewportWidget.PreviewMeshComponent);
				ViewportWidget.PreviewMeshComponent = nullptr;
			}

			UStaticMeshComponent* NewComponent = NewObject<UStaticMeshComponent>(GetTransientPackage(), NAME_None, RF_Transient);
			NewComponent->SetStaticMesh(PrimitiveMesh);
			if (ViewportWidget.PreviewMaterial)
			{
				NewComponent->SetMaterial(0, ViewportWidget.PreviewMaterial);
			}
			if (GEditor && GEditor->PreviewPlatform.GetEffectivePreviewFeatureLevel() <= ERHIFeatureLevel::ES3_1)
			{
				NewComponent->SetMobility(EComponentMobility::Static);
			}

			FTransform Transform = FTransform::Identity;
			if (PrimType == TPT_Plane)
			{
				Transform.SetRotation(FQuat(FRotator(0.0f, 180.0f, 0.0f)));
			}
			PreviewScene->AddComponent(NewComponent, Transform);
			ViewportWidget.PreviewMeshComponent = NewComponent;
			ViewportWidget.PreviewPrimType = PrimType;
			return true;
		}

		static bool ReadPreviewBackgroundChecked(SMaterialEditor3DPreviewViewport& ViewportWidget)
		{
			bool bBackground = false;
			TryReadCommandBool(ViewportWidget.GetCommandList(), FMaterialEditorCommands::Get().TogglePreviewBackground, bBackground);
			return bBackground;
		}

		static UObject* LoadMaterialPreviewAsset(FString AssetPath)
		{
			AssetPath.TrimStartAndEndInline();
			if (AssetPath.IsEmpty())
			{
				return nullptr;
			}
			UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
			if (!Asset && !AssetPath.Contains(TEXT(".")))
			{
				const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *AssetPath, *FPackageName::GetShortName(AssetPath));
				Asset = LoadObject<UObject>(nullptr, *ObjectPath);
			}
			if (Asset && (Asset->IsA<UMaterialInterface>() || Asset->IsA<UMaterialFunctionInterface>()))
			{
				return Asset;
			}
			return nullptr;
		}

		static bool IsMaterialPreviewAssetType(const FString& AssetType)
		{
			const FString Type = NormalizeToken(AssetType);
			return Type.IsEmpty()
				|| Type == TEXT("material")
				|| Type == TEXT("umaterial")
				|| Type == TEXT("material_interface")
				|| Type == TEXT("materialinterface")
				|| Type == TEXT("umaterialinterface")
				|| Type == TEXT("material_instance")
				|| Type == TEXT("materialinstance")
				|| Type == TEXT("material_instance_constant")
				|| Type == TEXT("materialinstanceconstant")
				|| Type == TEXT("umaterialinstance")
				|| Type == TEXT("umaterialinstanceconstant")
				|| Type == TEXT("material_function")
				|| Type == TEXT("materialfunction")
				|| Type == TEXT("material_function_interface")
				|| Type == TEXT("materialfunctioninterface")
				|| Type == TEXT("umaterialfunction")
				|| Type == TEXT("umaterialfunctioninterface");
		}

		static FString MaterialPreviewAssetType(const UObject* Asset)
		{
			if (!Asset)
			{
				return TEXT("material");
			}
			if (Asset->IsA<UMaterialFunctionInterface>())
			{
				return TEXT("material_function");
			}
			if (Asset->IsA<UMaterialInstance>())
			{
				return TEXT("material_instance");
			}
			if (Asset->IsA<UMaterial>())
			{
				return TEXT("material");
			}
			return NormalizeToken(Asset->GetClass() ? Asset->GetClass()->GetName() : FString());
		}

		static IAssetEditorInstance* GetMaterialAssetEditor(UObject* Asset, const bool bOpenIfNeeded, FString& OutError)
		{
			if (!Asset)
			{
				OutError = TEXT("material_asset_not_found");
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
				OutError = bOpenIfNeeded ? TEXT("material_editor_open_failed") : TEXT("material_editor_not_opened");
				return nullptr;
			}
			return AssetEditor;
		}

		static TSharedPtr<SWindow> ResolveMaterialEditorWindow(IAssetEditorInstance* AssetEditor, FString& OutError)
		{
			if (!AssetEditor)
			{
				OutError = TEXT("material_editor_not_opened");
				return nullptr;
			}
			const TSharedPtr<FTabManager> TabManager = AssetEditor->GetAssociatedTabManager();
			if (!TabManager.IsValid())
			{
				OutError = TEXT("material_tab_manager_not_found");
				return nullptr;
			}

			for (const FName PreviewTabId : { FName(TEXT("MaterialEditor_Preview")), FName(TEXT("MaterialInstanceEditor_Preview")) })
			{
				TSharedPtr<SDockTab> PreviewTab = TabManager->FindExistingLiveTab(FTabId(PreviewTabId));
				if (!PreviewTab.IsValid())
				{
					PreviewTab = TabManager->TryInvokeTab(FTabId(PreviewTabId), false);
				}
				if (PreviewTab.IsValid())
				{
					PreviewTab->ActivateInParent(ETabActivationCause::SetDirectly);
					PreviewTab->SlatePrepass();
				}
			}

			if (FSlateApplication::IsInitialized())
			{
				FSlateApplication::Get().Tick();
			}

			const TSharedPtr<SDockTab> OwnerTab = TabManager->GetOwnerTab();
			if (!OwnerTab.IsValid())
			{
				OutError = TEXT("material_owner_tab_not_found");
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
				OutError = TEXT("material_window_not_found");
			}
			return Window;
		}

		static TSharedPtr<SMaterialEditor3DPreviewViewport> ResolveMaterialViewportWidget(UObject* Asset, const bool bOpenIfNeeded, FString& OutError)
		{
			IAssetEditorInstance* AssetEditor = GetMaterialAssetEditor(Asset, bOpenIfNeeded, OutError);
			if (!AssetEditor)
			{
				return nullptr;
			}
			TSharedPtr<SWindow> Window = ResolveMaterialEditorWindow(AssetEditor, OutError);
			if (!Window.IsValid())
			{
				return nullptr;
			}

			TArray<TSharedRef<SWidget>> CandidateWidgets;
			FindWidgetsByTypeRecursive(Window.ToSharedRef(), FName(TEXT("SMaterialEditor3DPreviewViewport")), CandidateWidgets);
			TSharedPtr<SWidget> Widget = ChooseLargestWidget(CandidateWidgets);
			if (!Widget.IsValid())
			{
				OutError = TEXT("material_3d_preview_viewport_not_found");
				return nullptr;
			}
			TSharedPtr<SMaterialEditor3DPreviewViewport> ViewportWidget = StaticCastSharedPtr<SMaterialEditor3DPreviewViewport>(Widget);
			if (!ViewportWidget.IsValid() || !ViewportWidget->GetViewportClient().IsValid())
			{
				OutError = TEXT("material_3d_preview_viewport_invalid");
				return nullptr;
			}
			return ViewportWidget;
		}

		static bool ResolveMaterialRuntime(
			UObject* Asset,
			const bool bOpenIfNeeded,
			TSharedPtr<SMaterialEditor3DPreviewViewport>& OutViewportWidget,
			FEditorViewportClient*& OutClient,
			UMeshComponent*& OutPreviewMeshComponent,
			FString& OutError)
		{
			OutViewportWidget = ResolveMaterialViewportWidget(Asset, bOpenIfNeeded, OutError);
			if (!OutViewportWidget.IsValid())
			{
				return false;
			}
			const TSharedPtr<FEditorViewportClient> Client = OutViewportWidget->GetViewportClient();
			if (!Client.IsValid())
			{
				OutError = TEXT("material_viewport_client_not_found");
				return false;
			}
			OutClient = Client.Get();
			OutPreviewMeshComponent = OutViewportWidget->PreviewMeshComponent;
			return true;
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

		static TSharedPtr<FJsonObject> MakeMaterialVisualOptionsJson(FEditorViewportClient& Client)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("view_mode"), ViewModeToString(Client.GetViewMode()));
			Obj->SetNumberField(TEXT("view_mode_value"), static_cast<int32>(Client.GetViewMode()));
			TMap<FString, bool> Overlays;
			Overlays.Add(TEXT("grid"), Client.EngineShowFlags.Grid != 0);
			Overlays.Add(TEXT("bounds"), Client.EngineShowFlags.Bounds != 0);
			Obj->SetObjectField(TEXT("overlays"), MakeBoolOptionsJson(Overlays));
			Obj->SetBoolField(TEXT("realtime"), Client.IsRealtime());
			return Obj;
		}

		static void AddPreviewComponentSummary(UMeshComponent* PreviewComponent, TSharedPtr<FJsonObject>& Obj)
		{
			Obj->SetBoolField(TEXT("has_preview_component"), PreviewComponent != nullptr);
			if (!PreviewComponent)
			{
				return;
			}

			Obj->SetStringField(TEXT("preview_component_name"), PreviewComponent->GetName());
			Obj->SetStringField(TEXT("preview_component_class"), PreviewComponent->GetClass() ? PreviewComponent->GetClass()->GetName() : FString());
			if (const UStaticMeshComponent* StaticMeshComponent = Cast<UStaticMeshComponent>(PreviewComponent))
			{
				if (UStaticMesh* StaticMesh = StaticMeshComponent->GetStaticMesh())
				{
					Obj->SetStringField(TEXT("preview_mesh_asset_path"), StaticMesh->GetPathName());
				}
			}
			else if (const USkeletalMeshComponent* SkeletalMeshComponent = Cast<USkeletalMeshComponent>(PreviewComponent))
			{
				if (USkeletalMesh* SkeletalMesh = SkeletalMeshComponent->GetSkeletalMeshAsset())
				{
					Obj->SetStringField(TEXT("preview_mesh_asset_path"), SkeletalMesh->GetPathName());
				}
			}
			const FBox Bounds = PreviewComponent->Bounds.GetBox();
			if (Bounds.IsValid)
			{
				Obj->SetStringField(TEXT("bounds_space"), TEXT("material_preview_world"));
				Obj->SetObjectField(TEXT("preview_component_bounds"), MakeBoundsJson(Bounds));
			}
		}

		static TSharedPtr<FJsonObject> MakeMaterialPreviewStateJson(
			UObject* EditedAsset,
			FEditorViewportClient& Client,
			SMaterialEditor3DPreviewViewport& ViewportWidget,
			UMeshComponent* PreviewComponent)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("context"), TEXT("material_editor"));
			Obj->SetStringField(TEXT("asset_path"), EditedAsset ? EditedAsset->GetPathName() : FString());
			Obj->SetStringField(TEXT("asset_type"), MaterialPreviewAssetType(EditedAsset));
			Obj->SetStringField(TEXT("preview_primitive"), PreviewPrimitiveToString(ViewportWidget.PreviewPrimType));
			Obj->SetBoolField(TEXT("background"), ReadPreviewBackgroundChecked(ViewportWidget));
			Obj->SetBoolField(TEXT("post_process_material_preview"), Client.EngineShowFlags.PostProcessMaterial != 0);
			Obj->SetBoolField(TEXT("post_processing"), Client.EngineShowFlags.PostProcessing != 0);
			AddPreviewComponentSummary(PreviewComponent, Obj);
			return Obj;
		}

		static FBox ApplyPadding(const FBox& Box, const double Padding)
		{
			if (!Box.IsValid || Padding <= 1.0)
			{
				return Box;
			}
			const FVector Center = Box.GetCenter();
			const FVector Extent = Box.GetExtent() * static_cast<float>(Padding);
			return FBox::BuildAABB(Center, Extent);
		}

		class FMaterialViewportAdapter final : public IViewportAdapter
		{
		public:
			virtual FString GetName() const override
			{
				return TEXT("material");
			}

			virtual bool CanResolve(const FResolveRequest& Request) const override
			{
				return Request.Target.Scope == TEXT("asset_editor") && IsMaterialPreviewAssetType(Request.Target.AssetType);
			}

			virtual bool Resolve(const FResolveRequest& Request, FResolvedViewport& OutViewport, FString& OutError) const override
			{
				if (Request.Target.AssetPath.IsEmpty())
				{
					OutError = TEXT("missing_asset_path");
					return false;
				}
				UObject* Asset = LoadMaterialPreviewAsset(Request.Target.AssetPath);
				if (!Asset)
				{
					OutError = TEXT("material_asset_not_found");
					return false;
				}

				TSharedPtr<SMaterialEditor3DPreviewViewport> ViewportWidget;
				FEditorViewportClient* Client = nullptr;
				UMeshComponent* PreviewComponent = nullptr;
				if (!ResolveMaterialRuntime(Asset, Request.Target.bOpenEditorIfNeeded, ViewportWidget, Client, PreviewComponent, OutError))
				{
					return false;
				}

				OutViewport.Target = Request.Target;
				OutViewport.Target.Scope = TEXT("asset_editor");
				OutViewport.Target.AssetPath = Asset->GetPathName();
				OutViewport.Target.AssetType = MaterialPreviewAssetType(Asset);
				OutViewport.Target.ViewportRole = TEXT("preview");
				OutViewport.Target.ViewportId = FString::Printf(TEXT("asset:%s#material_preview"), *Asset->GetPathName());
				OutViewport.AdapterName = GetName();
				OutViewport.ClientClass = TEXT("FMaterialEditorViewportClient");
				OutViewport.ViewportClient = Client;
				OutViewport.Asset = Asset;
				return true;
			}

			virtual bool ReadState(const FResolvedViewport& Viewport, FViewportState& OutState, FString& OutError) const override
			{
				UObject* Asset = Viewport.Asset;
				TSharedPtr<SMaterialEditor3DPreviewViewport> ViewportWidget;
				FEditorViewportClient* Client = Viewport.ViewportClient;
				UMeshComponent* PreviewComponent = nullptr;
				if (!ResolveMaterialRuntime(Asset, false, ViewportWidget, Client, PreviewComponent, OutError))
				{
					return false;
				}

				OutState.Target = Viewport.Target;
				OutState.ClientClass = TEXT("FMaterialEditorViewportClient");
				OutState.Adapter = GetName();
				OutState.Camera.Location = Client->GetViewLocation();
				OutState.Camera.Rotation = Client->GetViewRotation();
				OutState.Camera.Fov = Client->ViewFOV;
				OutState.Camera.Space = TEXT("material_preview_world");
				OutState.Camera.Projection = Client->ViewportType == LVT_Perspective ? TEXT("perspective") : TEXT("orthographic");
				OutState.Camera.bValid = true;
				OutState.Capabilities.bCamera = true;
				OutState.Capabilities.bVisualOptions = true;
				OutState.Capabilities.bPreviewState = true;
				OutState.Capabilities.bScreenshot = true;
				OutState.Capabilities.FocusModes = { TEXT("preview_component_bounds"), TEXT("asset_bounds"), TEXT("bounds"), TEXT("selection"), TEXT("pivot"), TEXT("custom_bounds"), TEXT("custom_point") };
				OutState.Capabilities.ScreenshotBackends = { TEXT("material_editor_backbuffer") };
				OutState.Capabilities.VisualOptionKeys = { TEXT("view_mode"), TEXT("grid"), TEXT("bounds"), TEXT("realtime") };
				OutState.Capabilities.PreviewStateKeys = { TEXT("preview_primitive"), TEXT("background"), TEXT("post_process_material_preview") };
				OutState.VisualOptions = MakeMaterialVisualOptionsJson(*Client);
				OutState.PreviewState = MakeMaterialPreviewStateJson(Asset, *Client, *ViewportWidget, PreviewComponent);
				return true;
			}

			virtual bool WriteCamera(const FResolvedViewport& Viewport, const FViewportCamera& Camera, FString& OutError) const override
			{
				if (!Viewport.ViewportClient)
				{
					OutError = TEXT("invalid_material_viewport");
					return false;
				}
				Viewport.ViewportClient->SetViewLocation(Camera.Location);
				Viewport.ViewportClient->SetViewRotation(Camera.Rotation);
				Viewport.ViewportClient->ViewFOV = FMath::Clamp(Camera.Fov, 5.0f, 170.0f);
				Viewport.ViewportClient->Invalidate();
				if (Viewport.ViewportClient->Viewport)
				{
					Viewport.ViewportClient->Viewport->Invalidate();
				}
				return true;
			}

			virtual bool Focus(const FResolvedViewport& Viewport, const TSharedPtr<FJsonObject>& FocusObj, TSharedPtr<FJsonObject>& OutData, FString& OutError) const override
			{
				UObject* Asset = Viewport.Asset;
				TSharedPtr<SMaterialEditor3DPreviewViewport> ViewportWidget;
				FEditorViewportClient* Client = Viewport.ViewportClient;
				UMeshComponent* PreviewComponent = nullptr;
				if (!ResolveMaterialRuntime(Asset, false, ViewportWidget, Client, PreviewComponent, OutError))
				{
					return false;
				}
				if (!Client)
				{
					OutError = TEXT("invalid_material_viewport");
					return false;
				}

				FString Mode = TEXT("preview_component_bounds");
				TryGetStringField(FocusObj, TEXT("mode"), Mode);
				Mode = NormalizeToken(Mode);

				FBox FocusBox(ForceInit);
				FVector FocusPoint = FVector::ZeroVector;
				bool bUsePoint = false;
				if (Mode == TEXT("preview_component_bounds") || Mode == TEXT("asset_bounds") || Mode == TEXT("bounds") || Mode == TEXT("selection"))
				{
					if (!PreviewComponent)
					{
						OutError = TEXT("material_preview_component_not_found");
						return false;
					}
					FocusBox = PreviewComponent->Bounds.GetBox();
					if (Mode == TEXT("asset_bounds"))
					{
						Mode = TEXT("preview_component_bounds");
					}
				}
				else if (Mode == TEXT("pivot"))
				{
					FocusPoint = FVector::ZeroVector;
					bUsePoint = true;
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
					if (!TryReadVectorField(FocusObj, TEXT("point"), FocusPoint))
					{
						OutError = TEXT("missing_custom_point");
						return false;
					}
					bUsePoint = true;
				}
				else
				{
					OutError = TEXT("unsupported_material_focus_mode");
					return false;
				}

				double Radius = 50.0;
				TryGetNumberField(FocusObj, TEXT("radius"), Radius);
				bool bInstant = true;
				TryGetBoolField(FocusObj, TEXT("instant"), bInstant);
				if (bUsePoint)
				{
					FocusBox = FBox::BuildAABB(FocusPoint, FVector(FMath::Max(static_cast<float>(Radius), 1.0f)));
				}
				double Padding = 1.0;
				TryGetNumberField(FocusObj, TEXT("padding"), Padding);
				FocusBox = ApplyPadding(FocusBox, Padding);
				if (!FocusBox.IsValid)
				{
					OutError = TEXT("invalid_focus_bounds");
					return false;
				}

				Client->FocusViewportOnBox(FocusBox, bInstant);
				Client->SetLookAtLocation(FocusBox.GetCenter(), false);
				Client->Invalidate();
				if (Client->Viewport)
				{
					Client->Viewport->Invalidate();
				}

				OutData = MakeShared<FJsonObject>();
				OutData->SetStringField(TEXT("mode"), Mode);
				OutData->SetStringField(TEXT("space"), TEXT("material_preview_world"));
				OutData->SetStringField(TEXT("bounds_space"), TEXT("material_preview_world"));
				OutData->SetBoolField(TEXT("instant"), bInstant);
				OutData->SetObjectField(TEXT("bounds"), MakeBoundsJson(FocusBox));
				return true;
			}

			virtual bool ApplyVisualOptions(const FResolvedViewport& Viewport, const TSharedPtr<FJsonObject>& Options, TSharedPtr<FJsonObject>& OutData, FString& OutError) const override
			{
				if (!Viewport.ViewportClient)
				{
					OutError = TEXT("invalid_material_viewport");
					return false;
				}

				TArray<FString> Unsupported;
				FString ViewModeText;
				if (TryGetStringField(Options, TEXT("view_mode"), ViewModeText))
				{
					EViewModeIndex ViewMode = VMI_Lit;
					if (!TryParseViewMode(ViewModeText, ViewMode))
					{
						Unsupported.AddUnique(TEXT("view_mode"));
					}
					else
					{
						Viewport.ViewportClient->SetViewMode(ViewMode);
					}
				}

				bool bValue = false;
				if (TryReadBoolOption(Options, TEXT("grid"), bValue))
				{
					Viewport.ViewportClient->EngineShowFlags.SetGrid(bValue);
				}
				if (TryReadBoolOption(Options, TEXT("bounds"), bValue))
				{
					Viewport.ViewportClient->EngineShowFlags.SetBounds(bValue);
				}
				if (TryGetBoolField(Options, TEXT("realtime"), bValue))
				{
					Viewport.ViewportClient->SetRealtime(bValue);
				}

				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Options->Values)
				{
					const FString Key = NormalizeToken(Pair.Key);
					if (Key != TEXT("overlays") && Key != TEXT("view_mode") && Key != TEXT("view_mode_value") && Key != TEXT("grid") && Key != TEXT("bounds") && Key != TEXT("realtime") && Key != TEXT("ignore_unsupported"))
					{
						Unsupported.AddUnique(Pair.Key);
					}
				}
				TSharedPtr<FJsonObject> Overlays;
				if (TryGetObjectField(Options, TEXT("overlays"), Overlays))
				{
					for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Overlays->Values)
					{
						const FString Key = NormalizeToken(Pair.Key);
						if (Key != TEXT("grid") && Key != TEXT("bounds"))
						{
							Unsupported.AddUnique(Pair.Key);
						}
					}
				}

				Viewport.ViewportClient->Invalidate();
				if (Viewport.ViewportClient->Viewport)
				{
					Viewport.ViewportClient->Viewport->Invalidate();
				}
				OutData = MakeMaterialVisualOptionsJson(*Viewport.ViewportClient);
				if (Unsupported.Num() > 0)
				{
					OutData->SetArrayField(TEXT("unsupported_options"), MakeStringArrayJson(Unsupported));
					bool bIgnoreUnsupported = false;
					TryGetBoolField(Options, TEXT("ignore_unsupported"), bIgnoreUnsupported);
					if (!bIgnoreUnsupported)
					{
						OutError = TEXT("unsupported_visual_options");
						return false;
					}
				}
				return true;
			}

			virtual bool ApplyPreviewState(const FResolvedViewport& Viewport, const TSharedPtr<FJsonObject>& PreviewState, TSharedPtr<FJsonObject>& OutData, FString& OutError) const override
			{
				OutData = MakeShared<FJsonObject>();
				if (!HasUserFields(PreviewState))
				{
					OutData->SetBoolField(TEXT("applied"), false);
					OutData->SetStringField(TEXT("reason"), TEXT("empty_preview_state"));
					return true;
				}

				UObject* Asset = Viewport.Asset;
				TSharedPtr<SMaterialEditor3DPreviewViewport> ViewportWidget;
				FEditorViewportClient* Client = Viewport.ViewportClient;
				UMeshComponent* PreviewComponent = nullptr;
				if (!ResolveMaterialRuntime(Asset, false, ViewportWidget, Client, PreviewComponent, OutError))
				{
					return false;
				}

				TArray<FString> Unsupported;
				bool bApplied = false;
				FString PrimitiveText;
				if (TryGetStringField(PreviewState, TEXT("preview_primitive"), PrimitiveText) || TryGetStringField(PreviewState, TEXT("primitive"), PrimitiveText))
				{
					EThumbnailPrimType PrimType = TPT_None;
					if (!TryParsePreviewPrimitive(PrimitiveText, PrimType))
					{
						Unsupported.AddUnique(TEXT("preview_primitive"));
					}
					else if (ViewportWidget->PreviewPrimType != PrimType)
					{
						FString PrimitiveError;
						if (!SetPreviewPrimitiveTransient(*ViewportWidget, PrimType, PrimitiveError))
						{
							Unsupported.AddUnique(TEXT("preview_primitive"));
						}
						else
						{
							PreviewComponent = ViewportWidget->PreviewMeshComponent;
							bApplied = true;
						}
					}
				}

				bool bBackground = false;
				if (TryGetBoolField(PreviewState, TEXT("background"), bBackground))
				{
					if (ReadPreviewBackgroundChecked(*ViewportWidget) != bBackground)
					{
						if (!TrySetCommandBool(ViewportWidget->GetCommandList(), FMaterialEditorCommands::Get().TogglePreviewBackground, bBackground))
						{
							Unsupported.AddUnique(TEXT("background"));
						}
						else
						{
							bApplied = true;
						}
					}
				}

				bool bPostProcess = false;
				if (TryGetBoolField(PreviewState, TEXT("post_process"), bPostProcess)
					|| TryGetBoolField(PreviewState, TEXT("post_process_material_preview"), bPostProcess))
				{
					Client->EngineShowFlags.SetPostProcessing(bPostProcess);
					Client->EngineShowFlags.SetPostProcessMaterial(bPostProcess);
					bApplied = true;
				}

				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : PreviewState->Values)
				{
					const FString Key = NormalizeToken(Pair.Key);
					if (Key != TEXT("preview_primitive")
						&& Key != TEXT("primitive")
						&& Key != TEXT("background")
						&& Key != TEXT("post_process")
						&& Key != TEXT("post_process_material_preview")
						&& Key != TEXT("post_processing")
						&& Key != TEXT("context")
						&& Key != TEXT("asset_path")
						&& Key != TEXT("asset_type")
						&& Key != TEXT("has_preview_component")
						&& Key != TEXT("preview_component_name")
						&& Key != TEXT("preview_component_class")
						&& Key != TEXT("preview_mesh_asset_path")
						&& Key != TEXT("bounds_space")
						&& Key != TEXT("preview_component_bounds")
						&& Key != TEXT("ignore_unsupported"))
					{
						Unsupported.AddUnique(Pair.Key);
					}
				}

				Client->Invalidate();
				if (Client->Viewport)
				{
					Client->Viewport->Invalidate();
				}

				OutData->SetBoolField(TEXT("applied"), bApplied);
				OutData->SetStringField(TEXT("mode"), TEXT("material_preview_state"));
				OutData->SetObjectField(TEXT("preview_state"), MakeMaterialPreviewStateJson(Asset, *Client, *ViewportWidget, PreviewComponent));
				if (Unsupported.Num() > 0)
				{
					OutData->SetArrayField(TEXT("unsupported_options"), MakeStringArrayJson(Unsupported));
					bool bIgnoreUnsupported = false;
					TryGetBoolField(PreviewState, TEXT("ignore_unsupported"), bIgnoreUnsupported);
					if (!bIgnoreUnsupported)
					{
						OutError = TEXT("unsupported_material_preview_state");
						return false;
					}
				}
				return true;
			}

			virtual bool CaptureScreenshot(const FResolvedViewport& Viewport, const TSharedPtr<FJsonObject>& Params, FViewportScreenshotResult& OutResult, FString& OutError) const override
			{
				if (!Viewport.ViewportClient)
				{
					OutError = TEXT("invalid_material_viewport");
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
				OutResult.CaptureBackend = TEXT("material_editor_backbuffer");
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
				return ApplyPreviewState(Viewport, Snapshot.State.PreviewState, AppliedPreview, OutError);
			}
		};
	}

	TSharedRef<IViewportAdapter> MakeMaterialViewportAdapter()
	{
		return MakeShared<FMaterialViewportAdapter>();
	}
}
