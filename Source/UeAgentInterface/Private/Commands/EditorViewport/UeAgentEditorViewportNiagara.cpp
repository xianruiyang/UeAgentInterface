// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commands/EditorViewport/UeAgentEditorViewportNiagara.h"

#include "Editor.h"
#include "EditorViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Commands/UICommandList.h"
#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "NiagaraBakerSettings.h"
#include "Modules/ModuleManager.h"
#include "NiagaraComponent.h"
#include "NiagaraEditorCommands.h"
#include "NiagaraEditorModule.h"
#include "NiagaraEmitterInstance.h"
#include "NiagaraSimCache.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemInstance.h"
#include "NiagaraWorldManager.h"
#include "RenderingThread.h"
#include "SEditorViewport.h"
#include "Server/UeAgentHttpServer.h"
#include "ShowFlags.h"
#include "Slate/SceneViewport.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "UeAgentInterfaceSettings.h"
#include "Utilities/UeAgentJsonDiagnostics.h"
#include "ViewModels/NiagaraSystemViewModel.h"
#include "Widgets/Docking/SDockTab.h"

namespace UeAgentEditorViewport
{
	namespace
	{
		static constexpr const TCHAR* PreviewStateTagPrefix = TEXT("UAI_NiagaraPreviewState=");

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

		static bool TryReadBoolOption(const TSharedPtr<FJsonObject>& Options, const FString& Key, bool& OutValue)
		{
			if (TryGetBoolField(Options, Key, OutValue))
			{
				return true;
			}
			TSharedPtr<FJsonObject> Overlays;
			return TryGetObjectField(Options, TEXT("overlays"), Overlays) && TryGetBoolField(Overlays, Key, OutValue);
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

		template<typename TEnum>
		static FString EnumToString(const TEnum Value)
		{
			if (const UEnum* Enum = StaticEnum<TEnum>())
			{
				return Enum->GetNameStringByValue(static_cast<int64>(Value));
			}
			return FString::Printf(TEXT("%d"), static_cast<int32>(Value));
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

		static TSharedPtr<FJsonObject> MakeBoolOptionsJson(const TMap<FString, bool>& Values)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			for (const TPair<FString, bool>& Pair : Values)
			{
				Obj->SetBoolField(Pair.Key, Pair.Value);
			}
			return Obj;
		}

		static bool FindWidgetByTypeRecursive(const TSharedRef<SWidget>& RootWidget, const FName& TypeName, TSharedPtr<SWidget>& OutWidget)
		{
			if (RootWidget->GetType() == TypeName)
			{
				OutWidget = RootWidget;
				return true;
			}
			FChildren* Children = RootWidget->GetChildren();
			if (!Children)
			{
				return false;
			}
			for (int32 ChildIndex = 0; ChildIndex < Children->Num(); ++ChildIndex)
			{
				if (FindWidgetByTypeRecursive(Children->GetChildAt(ChildIndex), TypeName, OutWidget))
				{
					return true;
				}
			}
			return false;
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

		static void FindWidgetsByTypeRecursive(const TSharedRef<SWidget>& RootWidget, const FName& TypeName, TArray<TSharedRef<SWidget>>& OutWidgets)
		{
			if (RootWidget->GetType() == TypeName)
			{
				OutWidgets.Add(RootWidget);
			}
			FChildren* Children = RootWidget->GetChildren();
			if (!Children)
			{
				return;
			}
			for (int32 ChildIndex = 0; ChildIndex < Children->Num(); ++ChildIndex)
			{
				FindWidgetsByTypeRecursive(Children->GetChildAt(ChildIndex), TypeName, OutWidgets);
			}
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
			const FText OverrideSystem = NSLOCTEXT("UeAgentInterface", "NiagaraViewportScreenshotRealtimeOverrideSystem", "UeAgentInterfaceNiagaraViewportScreenshot");
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
			OutCaptureInfo->SetStringField(TEXT("backend"), TEXT("editor_viewport_backbuffer"));
			OutCaptureInfo->SetStringField(TEXT("adapter"), AdapterName);
			OutCaptureInfo->SetBoolField(TEXT("temporary_realtime_override"), bNeedTemporaryRealtime);
			OutCaptureInfo->SetNumberField(TEXT("draws_performed"), DrawsPerformed);
			OutCaptureInfo->SetBoolField(TEXT("applied_fallback_viewport_size"), bAppliedFallbackViewportSize);
			OutCaptureInfo->SetNumberField(TEXT("initial_viewport_width"), InitialViewportSize.X);
			OutCaptureInfo->SetNumberField(TEXT("initial_viewport_height"), InitialViewportSize.Y);
			return true;
		}

		static UNiagaraSystem* LoadNiagaraSystemAsset(FString AssetPath)
		{
			AssetPath.TrimStartAndEndInline();
			if (AssetPath.IsEmpty())
			{
				return nullptr;
			}
			if (UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *AssetPath))
			{
				return System;
			}
			if (!AssetPath.Contains(TEXT(".")))
			{
				const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *AssetPath, *FPackageName::GetShortName(AssetPath));
				return LoadObject<UNiagaraSystem>(nullptr, *ObjectPath);
			}
			return nullptr;
		}

		static bool IsNiagaraSystemAssetType(const FString& AssetType)
		{
			const FString Type = NormalizeToken(AssetType);
			return Type.IsEmpty()
				|| Type == TEXT("niagara")
				|| Type == TEXT("niagara_system")
				|| Type == TEXT("niagarasystem")
				|| Type == TEXT("uniagarasystem");
		}

		static FString NormalizeNiagaraViewportRole(const FString& Role)
		{
			const FString Value = NormalizeToken(Role);
			if (Value.IsEmpty()
				|| Value == TEXT("active")
				|| Value == TEXT("default")
				|| Value == TEXT("primary")
				|| Value == TEXT("preview")
				|| Value == TEXT("system")
				|| Value == TEXT("niagara_system"))
			{
				return TEXT("primary");
			}
			if (Value == TEXT("bake")
				|| Value == TEXT("baker")
				|| Value == TEXT("niagara_baker")
				|| Value == TEXT("baker_preview"))
			{
				return TEXT("baker");
			}
			if (Value == TEXT("cache")
				|| Value == TEXT("simcache")
				|| Value == TEXT("sim_cache")
				|| Value == TEXT("simulation_cache")
				|| Value == TEXT("niagara_sim_cache"))
			{
				return TEXT("sim_cache");
			}
			if (Value == TEXT("volume")
				|| Value == TEXT("volume_texture")
				|| Value == TEXT("texture_volume")
				|| Value == TEXT("sparse_volume_texture")
				|| Value == TEXT("animated_sparse_volume_texture")
				|| Value == TEXT("svt"))
			{
				return TEXT("volume_texture");
			}
			return Value;
		}

		static bool IsNiagaraPrimaryRole(const FString& Role)
		{
			return NormalizeNiagaraViewportRole(Role) == TEXT("primary");
		}

		static bool IsNiagaraBakerRole(const FString& Role)
		{
			return NormalizeNiagaraViewportRole(Role) == TEXT("baker");
		}

		static bool IsNiagaraSimCacheRole(const FString& Role)
		{
			return NormalizeNiagaraViewportRole(Role) == TEXT("sim_cache");
		}

		static bool IsNiagaraVolumeTextureRole(const FString& Role)
		{
			return NormalizeNiagaraViewportRole(Role) == TEXT("volume_texture");
		}

		static bool IsNiagaraSimCacheAssetType(const FString& AssetType)
		{
			const FString Type = NormalizeToken(AssetType);
			return Type == TEXT("niagara_sim_cache")
				|| Type == TEXT("niagarasimcache")
				|| Type == TEXT("uniagarasimcache")
				|| Type == TEXT("sim_cache")
				|| Type == TEXT("simcache")
				|| Type == TEXT("simulation_cache");
		}

		static bool IsNiagaraVolumeTextureAssetType(const FString& AssetType)
		{
			const FString Type = NormalizeToken(AssetType);
			return Type == TEXT("volume")
				|| Type == TEXT("volume_texture")
				|| Type == TEXT("volumetexture")
				|| Type == TEXT("uvolumetexture")
				|| Type == TEXT("sparse_volume_texture")
				|| Type == TEXT("sparsevolumetexture")
				|| Type == TEXT("usparsevolumetexture")
				|| Type == TEXT("animated_sparse_volume_texture")
				|| Type == TEXT("animatedsparsevolumetexture")
				|| Type == TEXT("uanimatedsparsevolumetexture");
		}

		static UNiagaraSimCache* LoadNiagaraSimCacheAsset(FString AssetPath)
		{
			AssetPath.TrimStartAndEndInline();
			if (AssetPath.IsEmpty())
			{
				return nullptr;
			}
			if (UNiagaraSimCache* SimCache = LoadObject<UNiagaraSimCache>(nullptr, *AssetPath))
			{
				return SimCache;
			}
			if (!AssetPath.Contains(TEXT(".")))
			{
				const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *AssetPath, *FPackageName::GetShortName(AssetPath));
				return LoadObject<UNiagaraSimCache>(nullptr, *ObjectPath);
			}
			return nullptr;
		}

		static bool TryGetStringFieldByAliases(const TSharedPtr<FJsonObject>& Obj, const TArray<FString>& Aliases, FString& OutValue)
		{
			for (const FString& Alias : Aliases)
			{
				if (TryGetStringField(Obj, Alias, OutValue))
				{
					return true;
				}
			}
			return false;
		}

		static UNiagaraSimCache* ResolveNiagaraSimCacheAsset(const FResolveRequest& Request, FString& OutError)
		{
			FString SimCachePath;
			if (IsNiagaraSimCacheAssetType(Request.Target.AssetType))
			{
				SimCachePath = Request.Target.AssetPath;
			}
			else
			{
				TryGetStringFieldByAliases(Request.Params, { TEXT("sim_cache_asset_path"), TEXT("cache_asset_path"), TEXT("sim_cache_path"), TEXT("cache_path") }, SimCachePath);
			}
			if (SimCachePath.TrimStartAndEnd().IsEmpty())
			{
				OutError = TEXT("missing_sim_cache_asset_path");
				return nullptr;
			}
			UNiagaraSimCache* SimCache = LoadNiagaraSimCacheAsset(SimCachePath);
			if (!SimCache)
			{
				OutError = TEXT("niagara_sim_cache_asset_not_found");
				return nullptr;
			}
			if (!SimCache->IsCacheValid())
			{
				OutError = TEXT("niagara_sim_cache_invalid_or_empty");
				return nullptr;
			}
			if (!SimCache->GetSystem(true))
			{
				OutError = TEXT("niagara_sim_cache_system_not_found");
				return nullptr;
			}
			return SimCache;
		}

		static UNiagaraSystem* ResolveNiagaraSystemFromViewportAsset(const FResolvedViewport& Viewport, UNiagaraSimCache*& OutSimCache, FString& OutError)
		{
			OutSimCache = Cast<UNiagaraSimCache>(Viewport.Asset);
			if (UNiagaraSystem* NiagaraSystem = Cast<UNiagaraSystem>(Viewport.Asset))
			{
				return NiagaraSystem;
			}
			if (OutSimCache)
			{
				if (UNiagaraSystem* NiagaraSystem = OutSimCache->GetSystem(true))
				{
					return NiagaraSystem;
				}
				OutError = TEXT("niagara_sim_cache_system_not_found");
				return nullptr;
			}
			OutError = TEXT("invalid_niagara_viewport_asset");
			return nullptr;
		}

		static int32 GetBakerFramesPerSecond(UNiagaraSystem* NiagaraSystem)
		{
			const UNiagaraBakerSettings* BakerSettings = NiagaraSystem ? NiagaraSystem->GetBakerSettings() : nullptr;
			return BakerSettings ? FMath::Clamp(BakerSettings->FramesPerSecond, 1, 480) : 60;
		}

		static float GetBakerStartSeconds(UNiagaraSystem* NiagaraSystem)
		{
			const UNiagaraBakerSettings* BakerSettings = NiagaraSystem ? NiagaraSystem->GetBakerSettings() : nullptr;
			return BakerSettings ? BakerSettings->StartSeconds : 0.0f;
		}

		static float GetBakerDurationSeconds(UNiagaraSystem* NiagaraSystem)
		{
			const UNiagaraBakerSettings* BakerSettings = NiagaraSystem ? NiagaraSystem->GetBakerSettings() : nullptr;
			return BakerSettings ? FMath::Max(BakerSettings->DurationSeconds, 0.0f) : 0.0f;
		}

		static void FlushNiagaraPreviewWorld(UNiagaraComponent* PreviewComponent)
		{
			if (!PreviewComponent)
			{
				return;
			}
			if (UWorld* PreviewWorld = PreviewComponent->GetWorld())
			{
				PreviewWorld->SendAllEndOfFrameUpdates();
				if (FNiagaraWorldManager* WorldManager = FNiagaraWorldManager::Get(PreviewWorld))
				{
					WorldManager->FlushComputeAndDeferredQueues(false);
				}
			}
		}

		static FString GetPreviewStateToken(const UNiagaraComponent* PreviewComponent)
		{
			if (!PreviewComponent)
			{
				return FString();
			}
			for (const FName& TagName : PreviewComponent->ComponentTags)
			{
				const FString Tag = TagName.ToString();
				if (Tag.StartsWith(PreviewStateTagPrefix))
				{
					return Tag.RightChop(FCString::Strlen(PreviewStateTagPrefix));
				}
			}
			return FString();
		}

		static void SetPreviewStateToken(UNiagaraComponent* PreviewComponent, const FString& Token)
		{
			if (!PreviewComponent)
			{
				return;
			}
			for (int32 Index = PreviewComponent->ComponentTags.Num() - 1; Index >= 0; --Index)
			{
				if (PreviewComponent->ComponentTags[Index].ToString().StartsWith(PreviewStateTagPrefix))
				{
					PreviewComponent->ComponentTags.RemoveAt(Index);
				}
			}
			if (!Token.IsEmpty())
			{
				PreviewComponent->ComponentTags.AddUnique(FName(*(FString(PreviewStateTagPrefix) + Token)));
			}
		}

		static FString MakePreviewStateToken(const UNiagaraSystem* NiagaraSystem, const UNiagaraComponent* PreviewComponent, const int32 FrameIndex, const float TickDeltaSeconds, const FString& AdvanceMode)
		{
			const FString AssetPath = NiagaraSystem ? NiagaraSystem->GetOutermost()->GetName() : FString();
			const FString ComponentPath = PreviewComponent ? PreviewComponent->GetPathName() : FString();
			const FString WorldPath = (PreviewComponent && PreviewComponent->GetWorld()) ? PreviewComponent->GetWorld()->GetPathName() : FString();
			return FString::Printf(
				TEXT("asset=%s;component=%s;world=%s;frame=%d;dt=%.9f;mode=%s;semantics=continuous_from_zero;nonce=%s"),
				*AssetPath,
				*ComponentPath,
				*WorldPath,
				FrameIndex,
				TickDeltaSeconds,
				*AdvanceMode,
				*FGuid::NewGuid().ToString(EGuidFormats::Digits));
		}

		static FString MakeAbsolutePreviewStateToken(
			const UNiagaraSystem* NiagaraSystem,
			const UNiagaraComponent* PreviewComponent,
			const FString& Role,
			const FString& Mode,
			const float AbsoluteTimeSeconds,
			const FString& SourceAssetPath)
		{
			const FString AssetPath = NiagaraSystem ? NiagaraSystem->GetOutermost()->GetName() : FString();
			const FString ComponentPath = PreviewComponent ? PreviewComponent->GetPathName() : FString();
			const FString WorldPath = (PreviewComponent && PreviewComponent->GetWorld()) ? PreviewComponent->GetWorld()->GetPathName() : FString();
			return FString::Printf(
				TEXT("asset=%s;source=%s;component=%s;world=%s;role=%s;time=%.9f;mode=%s;semantics=absolute_time;nonce=%s"),
				*AssetPath,
				*SourceAssetPath,
				*ComponentPath,
				*WorldPath,
				*Role,
				AbsoluteTimeSeconds,
				*Mode,
				*FGuid::NewGuid().ToString(EGuidFormats::Digits));
		}

		static bool GetRuntimePosition(UNiagaraComponent* PreviewComponent, const float TickDeltaSeconds, double& OutSystemAge, int32& OutSystemTickCount, int32& OutFrameEstimate)
		{
			OutSystemAge = -1.0;
			OutSystemTickCount = INDEX_NONE;
			OutFrameEstimate = INDEX_NONE;
			if (!PreviewComponent)
			{
				return false;
			}
PRAGMA_DISABLE_DEPRECATION_WARNINGS
			FNiagaraSystemInstance* SystemInstance = PreviewComponent->GetSystemInstance();
PRAGMA_ENABLE_DEPRECATION_WARNINGS
			if (!SystemInstance)
			{
				return false;
			}
			OutSystemAge = SystemInstance->GetAge();
			OutSystemTickCount = SystemInstance->GetTickCount();
			OutFrameEstimate = TickDeltaSeconds > SMALL_NUMBER ? FMath::RoundToInt(static_cast<float>(OutSystemAge) / TickDeltaSeconds) : INDEX_NONE;
			return true;
		}

		static TSharedPtr<FJsonObject> MakeParticleStatsJson(UNiagaraComponent* PreviewComponent)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetBoolField(TEXT("available"), false);
			if (!PreviewComponent)
			{
				return Obj;
			}
PRAGMA_DISABLE_DEPRECATION_WARNINGS
			FNiagaraSystemInstance* SystemInstance = PreviewComponent->GetSystemInstance();
PRAGMA_ENABLE_DEPRECATION_WARNINGS
			if (!SystemInstance)
			{
				return Obj;
			}
			int32 TotalParticles = 0;
			int32 TotalSpawnedParticles = 0;
			TArray<TSharedPtr<FJsonValue>> Emitters;
			for (const FNiagaraEmitterInstanceRef& EmitterRef : SystemInstance->GetEmitters())
			{
				FNiagaraEmitterInstance& EmitterInstance = EmitterRef.Get();
				const int32 NumParticles = EmitterInstance.GetNumParticles();
				const int32 SpawnedParticles = EmitterInstance.GetTotalSpawnedParticles();
				TotalParticles += NumParticles;
				TotalSpawnedParticles += SpawnedParticles;

				TSharedPtr<FJsonObject> EmitterObj = MakeShared<FJsonObject>();
				EmitterObj->SetStringField(TEXT("name"), EmitterInstance.GetEmitterHandle().GetName().ToString());
				EmitterObj->SetNumberField(TEXT("particle_count"), NumParticles);
				EmitterObj->SetNumberField(TEXT("total_spawned_particles"), SpawnedParticles);
				Emitters.Add(MakeShared<FJsonValueObject>(EmitterObj));
			}
			Obj->SetBoolField(TEXT("available"), true);
			Obj->SetNumberField(TEXT("emitter_instance_count"), Emitters.Num());
			Obj->SetNumberField(TEXT("total_particle_count"), TotalParticles);
			Obj->SetNumberField(TEXT("total_spawned_particles"), TotalSpawnedParticles);
			Obj->SetArrayField(TEXT("emitters"), Emitters);
			return Obj;
		}

		static void ConfigurePreviewComponentForFrame(UNiagaraComponent* Component, UNiagaraSystem* NiagaraSystem, const float TickDeltaSeconds)
		{
			if (!Component)
			{
				return;
			}
			Component->SetAsset(NiagaraSystem, false);
			Component->SetForceSolo(true);
			Component->SetAgeUpdateMode(ENiagaraAgeUpdateMode::TickDeltaTime);
			Component->SetSeekDelta(TickDeltaSeconds);
			Component->SetLockDesiredAgeDeltaTimeToSeekDelta(true);
			Component->SetCanRenderWhileSeeking(true);
			Component->SetAllowScalability(false);
			Component->SetRenderingEnabled(true);
		}

		struct FNiagaraTemporalSeek
		{
			bool bValid = false;
			FString Mode;
			float AbsoluteTimeSeconds = 0.0f;
			float NormalizedTime = -1.0f;
			int32 SourceFrame = INDEX_NONE;
			float TickDeltaSeconds = 1.0f / 60.0f;
		};

		static bool TryResolveNiagaraTemporalSeek(
			const TSharedPtr<FJsonObject>& PreviewState,
			const FString& ViewportRole,
			UNiagaraSystem* NiagaraSystem,
			UNiagaraSimCache* SimCache,
			const float TickDeltaSeconds,
			FNiagaraTemporalSeek& OutSeek)
		{
			OutSeek = FNiagaraTemporalSeek();
			OutSeek.TickDeltaSeconds = TickDeltaSeconds;
			double AbsoluteTimeNumber = 0.0;
			if (UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(
				PreviewState,
				{ TEXT("absolute_time"), TEXT("absolute_time_seconds"), TEXT("time_seconds"), TEXT("seconds"), TEXT("desired_age") },
				AbsoluteTimeNumber))
			{
				OutSeek.bValid = true;
				OutSeek.Mode = TEXT("absolute_time");
				OutSeek.AbsoluteTimeSeconds = FMath::Max(static_cast<float>(AbsoluteTimeNumber), 0.0f);
				return true;
			}

			double NormalizedNumber = 0.0;
			if (UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(PreviewState, { TEXT("normalized_time"), TEXT("normalized") }, NormalizedNumber))
			{
				const float NormalizedTime = FMath::Clamp(static_cast<float>(NormalizedNumber), 0.0f, 1.0f);
				float StartSeconds = 0.0f;
				float DurationSeconds = 0.0f;
				if (SimCache)
				{
					StartSeconds = SimCache->GetStartSeconds();
					DurationSeconds = SimCache->GetDurationSeconds();
				}
				else if (IsNiagaraBakerRole(ViewportRole))
				{
					StartSeconds = GetBakerStartSeconds(NiagaraSystem);
					DurationSeconds = GetBakerDurationSeconds(NiagaraSystem);
				}
				OutSeek.bValid = true;
				OutSeek.Mode = TEXT("normalized_time");
				OutSeek.NormalizedTime = NormalizedTime;
				OutSeek.AbsoluteTimeSeconds = FMath::Max(StartSeconds + DurationSeconds * NormalizedTime, 0.0f);
				return true;
			}

			double FrameNumber = 0.0;
			bool bHasFrame = false;
			if (SimCache)
			{
				bHasFrame = UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(
					PreviewState,
					{ TEXT("cache_frame"), TEXT("sim_cache_frame"), TEXT("frame"), TEXT("target_frame") },
					FrameNumber);
				if (bHasFrame)
				{
					const int32 MaxFrameIndex = FMath::Max(SimCache->GetNumFrames() - 1, 0);
					const int32 FrameIndex = FMath::Clamp(FMath::RoundToInt(FrameNumber), 0, MaxFrameIndex);
					const float Denominator = FMath::Max(static_cast<float>(MaxFrameIndex), 1.0f);
					const float NormalizedTime = MaxFrameIndex > 0 ? static_cast<float>(FrameIndex) / Denominator : 0.0f;
					OutSeek.bValid = true;
					OutSeek.Mode = TEXT("sim_cache_frame");
					OutSeek.SourceFrame = FrameIndex;
					OutSeek.NormalizedTime = NormalizedTime;
					OutSeek.AbsoluteTimeSeconds = FMath::Max(SimCache->GetStartSeconds() + SimCache->GetDurationSeconds() * NormalizedTime, 0.0f);
					return true;
				}
			}

			if (IsNiagaraBakerRole(ViewportRole))
			{
				bHasFrame = UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(
					PreviewState,
					{ TEXT("baker_frame"), TEXT("output_frame"), TEXT("frame"), TEXT("target_frame") },
					FrameNumber);
				if (bHasFrame)
				{
					const int32 FrameIndex = FMath::Clamp(FMath::RoundToInt(FrameNumber), 0, 14400);
					const int32 FramesPerSecond = GetBakerFramesPerSecond(NiagaraSystem);
					OutSeek.bValid = true;
					OutSeek.Mode = TEXT("baker_frame");
					OutSeek.SourceFrame = FrameIndex;
					OutSeek.AbsoluteTimeSeconds = FMath::Max(GetBakerStartSeconds(NiagaraSystem) + static_cast<float>(FrameIndex) / static_cast<float>(FramesPerSecond), 0.0f);
					const float DurationSeconds = GetBakerDurationSeconds(NiagaraSystem);
					if (DurationSeconds > SMALL_NUMBER)
					{
						OutSeek.NormalizedTime = FMath::Clamp((OutSeek.AbsoluteTimeSeconds - GetBakerStartSeconds(NiagaraSystem)) / DurationSeconds, 0.0f, 1.0f);
					}
					return true;
				}
			}

			return false;
		}

		static TSharedPtr<FUICommandInfo> NiagaraCommand(const TSharedPtr<FUICommandInfo>& Command)
		{
			return Command;
		}

		static bool TryReadCommandBool(const TSharedPtr<FUICommandList>& CommandList, const TSharedPtr<FUICommandInfo>& Command, bool& OutValue)
		{
			if (!CommandList.IsValid() || !Command.IsValid())
			{
				return false;
			}
			const FUIAction* Action = CommandList->GetActionForCommand(Command);
			if (!Action || !Action->GetActionCheckState.IsBound())
			{
				return false;
			}
			OutValue = Action->GetActionCheckState.Execute() == ECheckBoxState::Checked;
			return true;
		}

		static bool TrySetCommandBool(const TSharedPtr<FUICommandList>& CommandList, const TSharedPtr<FUICommandInfo>& Command, const bool bValue)
		{
			bool bCurrent = false;
			if (!TryReadCommandBool(CommandList, Command, bCurrent))
			{
				return false;
			}
			if (bCurrent != bValue)
			{
				return CommandList->TryExecuteAction(Command.ToSharedRef());
			}
			return true;
		}

		static TSharedPtr<FJsonObject> MakeNiagaraVisualOptionsJson(FEditorViewportClient& Client, const TSharedPtr<SEditorViewport>& ViewportWidget)
		{
			const TSharedPtr<FUICommandList> CommandList = ViewportWidget.IsValid() ? ViewportWidget->GetCommandList() : nullptr;
			const FNiagaraEditorCommands& Commands = FNiagaraEditorCommands::Get();
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("view_mode"), ViewModeToString(Client.GetViewMode()));
			Obj->SetNumberField(TEXT("view_mode_value"), static_cast<int32>(Client.GetViewMode()));
			TMap<FString, bool> Overlays;
			bool bCommandValue = false;
			Overlays.Add(TEXT("grid"), Client.EngineShowFlags.Grid != 0);
			if (TryReadCommandBool(CommandList, NiagaraCommand(Commands.TogglePreviewGrid), bCommandValue))
			{
				Overlays.Add(TEXT("grid"), bCommandValue);
			}
			Overlays.Add(TEXT("bounds"), Client.EngineShowFlags.Bounds != 0);
			if (TryReadCommandBool(CommandList, NiagaraCommand(Commands.ToggleOriginAxis), bCommandValue))
			{
				Overlays.Add(TEXT("origin_axis"), bCommandValue);
			}
			if (TryReadCommandBool(CommandList, NiagaraCommand(Commands.ToggleParticleCounts), bCommandValue))
			{
				Overlays.Add(TEXT("particle_counts"), bCommandValue);
			}
			if (TryReadCommandBool(CommandList, NiagaraCommand(Commands.TogglePreviewBackground), bCommandValue))
			{
				Overlays.Add(TEXT("background"), bCommandValue);
			}
			if (TryReadCommandBool(CommandList, NiagaraCommand(Commands.ToggleInstructionCounts), bCommandValue))
			{
				Overlays.Add(TEXT("instruction_counts"), bCommandValue);
			}
			Obj->SetObjectField(TEXT("overlays"), MakeBoolOptionsJson(Overlays));
			Obj->SetBoolField(TEXT("realtime"), Client.IsRealtime());
			return Obj;
		}

		static TSharedPtr<FJsonObject> MakeNiagaraPreviewStateJson(
			UNiagaraSystem& NiagaraSystem,
			UNiagaraComponent* PreviewComponent,
			const FString& ViewportRole = TEXT("primary"),
			UNiagaraSimCache* SimCache = nullptr)
		{
			const FString NormalizedRole = NormalizeNiagaraViewportRole(ViewportRole);
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("context"), NormalizedRole == TEXT("sim_cache") ? TEXT("niagara_sim_cache") : (NormalizedRole == TEXT("baker") ? TEXT("niagara_baker") : TEXT("niagara_system")));
			Obj->SetStringField(TEXT("viewport_role"), NormalizedRole);
			Obj->SetStringField(TEXT("asset_path"), NiagaraSystem.GetOutermost()->GetName());
			Obj->SetStringField(TEXT("object_path"), NiagaraSystem.GetPathName());
			Obj->SetBoolField(TEXT("system_ready_to_run"), NiagaraSystem.IsReadyToRun());
			Obj->SetBoolField(TEXT("system_has_outstanding_compilation_requests"), NiagaraSystem.HasOutstandingCompilationRequests(true));
			if (const UNiagaraBakerSettings* BakerSettings = NiagaraSystem.GetBakerSettings())
			{
				TSharedPtr<FJsonObject> BakerObj = MakeShared<FJsonObject>();
				BakerObj->SetNumberField(TEXT("start_seconds"), BakerSettings->StartSeconds);
				BakerObj->SetNumberField(TEXT("duration_seconds"), BakerSettings->DurationSeconds);
				BakerObj->SetNumberField(TEXT("frames_per_second"), BakerSettings->FramesPerSecond);
				BakerObj->SetNumberField(TEXT("current_camera_index"), BakerSettings->CurrentCameraIndex);
				BakerObj->SetBoolField(TEXT("preview_looping"), BakerSettings->bPreviewLooping != 0);
				BakerObj->SetBoolField(TEXT("render_component_only"), BakerSettings->bRenderComponentOnly != 0);
				Obj->SetObjectField(TEXT("baker"), BakerObj);
			}
			if (SimCache)
			{
				TSharedPtr<FJsonObject> SimCacheObj = MakeShared<FJsonObject>();
				SimCacheObj->SetStringField(TEXT("asset_path"), SimCache->GetOutermost()->GetName());
				SimCacheObj->SetStringField(TEXT("object_path"), SimCache->GetPathName());
				SimCacheObj->SetBoolField(TEXT("valid"), SimCache->IsCacheValid());
				SimCacheObj->SetNumberField(TEXT("num_frames"), SimCache->GetNumFrames());
				SimCacheObj->SetNumberField(TEXT("num_emitters"), SimCache->GetNumEmitters());
				SimCacheObj->SetNumberField(TEXT("start_seconds"), SimCache->GetStartSeconds());
				SimCacheObj->SetNumberField(TEXT("duration_seconds"), SimCache->GetDurationSeconds());
				Obj->SetObjectField(TEXT("sim_cache"), SimCacheObj);
			}
			if (!PreviewComponent)
			{
				Obj->SetBoolField(TEXT("has_preview_component"), false);
				return Obj;
			}
			const float TickDeltaSeconds = FMath::Clamp(PreviewComponent->GetSeekDelta(), 1.0f / 240.0f, 1.0f / 5.0f);
			double SystemAge = -1.0;
			int32 SystemTickCount = INDEX_NONE;
			int32 FrameEstimate = INDEX_NONE;
			const bool bHasRuntimePosition = GetRuntimePosition(PreviewComponent, TickDeltaSeconds, SystemAge, SystemTickCount, FrameEstimate);
			Obj->SetBoolField(TEXT("has_preview_component"), true);
			Obj->SetStringField(TEXT("component_name"), PreviewComponent->GetName());
			Obj->SetStringField(TEXT("component_path"), PreviewComponent->GetPathName());
			Obj->SetStringField(TEXT("component_world"), PreviewComponent->GetWorld() ? PreviewComponent->GetWorld()->GetPathName() : FString());
			Obj->SetBoolField(TEXT("component_registered"), PreviewComponent->IsRegistered());
			Obj->SetBoolField(TEXT("component_active"), PreviewComponent->IsActive());
			Obj->SetBoolField(TEXT("component_paused"), PreviewComponent->IsPaused());
			Obj->SetStringField(TEXT("age_update_mode"), EnumToString(PreviewComponent->GetAgeUpdateMode()));
			Obj->SetNumberField(TEXT("desired_age"), PreviewComponent->GetDesiredAge());
			Obj->SetNumberField(TEXT("tick_delta_seconds"), TickDeltaSeconds);
			if (UNiagaraSimCache* ActiveSimCache = PreviewComponent->GetSimCache())
			{
				Obj->SetStringField(TEXT("active_sim_cache_asset_path"), ActiveSimCache->GetOutermost()->GetName());
				Obj->SetStringField(TEXT("active_sim_cache_object_path"), ActiveSimCache->GetPathName());
			}
			Obj->SetBoolField(TEXT("has_runtime_position"), bHasRuntimePosition);
			Obj->SetNumberField(TEXT("system_age"), SystemAge);
			Obj->SetNumberField(TEXT("system_tick_count"), SystemTickCount);
			Obj->SetNumberField(TEXT("current_frame_estimate"), FrameEstimate);
			Obj->SetStringField(TEXT("preview_state_token"), GetPreviewStateToken(PreviewComponent));
			Obj->SetObjectField(TEXT("particle_stats"), MakeParticleStatsJson(PreviewComponent));
			return Obj;
		}

		class FNiagaraViewportAdapter final : public IViewportAdapter
		{
		public:
			virtual FString GetName() const override
			{
				return TEXT("niagara_system");
			}

			virtual bool CanResolve(const FResolveRequest& Request) const override
			{
				if (Request.Target.Scope != TEXT("asset_editor"))
				{
					return false;
				}
				if (IsNiagaraVolumeTextureRole(Request.Target.ViewportRole) || IsNiagaraVolumeTextureAssetType(Request.Target.AssetType))
				{
					return true;
				}
				if (IsNiagaraSimCacheRole(Request.Target.ViewportRole) || IsNiagaraSimCacheAssetType(Request.Target.AssetType))
				{
					return true;
				}
				return IsNiagaraSystemAssetType(Request.Target.AssetType)
					&& (IsNiagaraPrimaryRole(Request.Target.ViewportRole) || IsNiagaraBakerRole(Request.Target.ViewportRole));
			}

			virtual bool Resolve(const FResolveRequest& Request, FResolvedViewport& OutViewport, FString& OutError) const override
			{
				if (Request.Target.AssetPath.IsEmpty())
				{
					OutError = TEXT("missing_asset_path");
					return false;
				}
				const FString ViewportRole = NormalizeNiagaraViewportRole(Request.Target.ViewportRole);
				if (ViewportRole == TEXT("volume_texture") || IsNiagaraVolumeTextureAssetType(Request.Target.AssetType))
				{
					OutError = TEXT("volume_texture_preview_requires_special_texture_viewport_adapter");
					return false;
				}

				UNiagaraSimCache* SimCache = nullptr;
				UNiagaraSystem* NiagaraSystem = nullptr;
				if (ViewportRole == TEXT("sim_cache") || IsNiagaraSimCacheAssetType(Request.Target.AssetType))
				{
					SimCache = ResolveNiagaraSimCacheAsset(Request, OutError);
					if (!SimCache)
					{
						return false;
					}
					NiagaraSystem = SimCache->GetSystem(true);
				}
				else
				{
					NiagaraSystem = LoadNiagaraSystemAsset(Request.Target.AssetPath);
				}
				if (!NiagaraSystem)
				{
					OutError = TEXT("niagara_system_asset_not_found");
					return false;
				}
				TSharedPtr<FNiagaraSystemViewModel> SystemViewModel;
				UNiagaraComponent* PreviewComponent = nullptr;
				TSharedPtr<SEditorViewport> ViewportWidget = ResolveNiagaraViewportWidget(NiagaraSystem, Request.Target.bOpenEditorIfNeeded, SystemViewModel, PreviewComponent, OutError);
				if (!ViewportWidget.IsValid())
				{
					return false;
				}
				TSharedPtr<FEditorViewportClient> Client = ViewportWidget->GetViewportClient();
				if (!Client.IsValid())
				{
					OutError = TEXT("niagara_viewport_client_not_found");
					return false;
				}
				if (SimCache)
				{
					ConfigurePreviewComponentForFrame(PreviewComponent, NiagaraSystem, 1.0f / 60.0f);
					PreviewComponent->SetSimCache(SimCache, true);
					PreviewComponent->SetPaused(false);
					PreviewComponent->Activate(false);
					PreviewComponent->SeekToDesiredAge(SimCache->GetStartSeconds());
					PreviewComponent->TickComponent(0.0f, ELevelTick::LEVELTICK_All, nullptr);
					FlushNiagaraPreviewWorld(PreviewComponent);
				}

				OutViewport.Target = Request.Target;
				OutViewport.Target.Scope = TEXT("asset_editor");
				OutViewport.Target.AssetPath = SimCache ? SimCache->GetOutermost()->GetName() : NiagaraSystem->GetOutermost()->GetName();
				OutViewport.Target.AssetType = SimCache ? TEXT("niagara_sim_cache") : TEXT("niagara_system");
				OutViewport.Target.ViewportRole = ViewportRole;
				OutViewport.Target.ViewportId = FString::Printf(
					TEXT("asset:%s#niagara_%s"),
					*(SimCache ? SimCache->GetPathName() : NiagaraSystem->GetPathName()),
					*ViewportRole);
				OutViewport.AdapterName = GetName();
				OutViewport.ClientClass = TEXT("FNiagaraSystemViewportClient");
				OutViewport.ViewportClient = Client.Get();
				OutViewport.Asset = SimCache ? static_cast<UObject*>(SimCache) : static_cast<UObject*>(NiagaraSystem);
				return true;
			}

			virtual bool ReadState(const FResolvedViewport& Viewport, FViewportState& OutState, FString& OutError) const override
			{
				UNiagaraSimCache* SimCache = nullptr;
				UNiagaraSystem* NiagaraSystem = ResolveNiagaraSystemFromViewportAsset(Viewport, SimCache, OutError);
				if (!NiagaraSystem)
				{
					return false;
				}
				TSharedPtr<FNiagaraSystemViewModel> SystemViewModel;
				UNiagaraComponent* PreviewComponent = nullptr;
				TSharedPtr<SEditorViewport> ViewportWidget;
				FEditorViewportClient* Client = Viewport.ViewportClient;
				if (!ResolveNiagaraRuntime(NiagaraSystem, false, SystemViewModel, PreviewComponent, ViewportWidget, Client, OutError))
				{
					return false;
				}
				OutState.Target = Viewport.Target;
				OutState.ClientClass = TEXT("FNiagaraSystemViewportClient");
				OutState.Adapter = GetName();
				OutState.Camera.Location = Client->GetViewLocation();
				OutState.Camera.Rotation = Client->GetViewRotation();
				OutState.Camera.Fov = Client->ViewFOV;
				OutState.Camera.Space = TEXT("niagara_preview_world");
				OutState.Camera.Projection = Client->ViewportType == LVT_Perspective ? TEXT("perspective") : TEXT("orthographic");
				OutState.Camera.bValid = true;
				OutState.Capabilities.bCamera = true;
				OutState.Capabilities.bVisualOptions = true;
				OutState.Capabilities.bPreviewState = true;
				OutState.Capabilities.bScreenshot = true;
				OutState.Capabilities.FocusModes = { TEXT("preview_component_bounds"), TEXT("asset_bounds"), TEXT("bounds"), TEXT("selection"), TEXT("custom_bounds"), TEXT("custom_point") };
				if (Viewport.Target.ViewportRole == TEXT("baker"))
				{
					OutState.Capabilities.ScreenshotBackends = { TEXT("niagara_baker_editor_backbuffer") };
				}
				else if (Viewport.Target.ViewportRole == TEXT("sim_cache"))
				{
					OutState.Capabilities.ScreenshotBackends = { TEXT("niagara_sim_cache_editor_backbuffer") };
				}
				else
				{
					OutState.Capabilities.ScreenshotBackends = { TEXT("niagara_editor_backbuffer") };
				}
				OutState.Capabilities.VisualOptionKeys = { TEXT("view_mode"), TEXT("grid"), TEXT("bounds"), TEXT("origin_axis"), TEXT("particle_counts"), TEXT("background"), TEXT("instruction_counts"), TEXT("realtime") };
				if (Viewport.Target.ViewportRole == TEXT("baker"))
				{
					OutState.Capabilities.PreviewStateKeys = { TEXT("absolute_time"), TEXT("time_seconds"), TEXT("normalized_time"), TEXT("baker_frame"), TEXT("frame"), TEXT("tick_delta_seconds"), TEXT("pause"), TEXT("reset"), TEXT("restore_interactive_preview") };
				}
				else if (Viewport.Target.ViewportRole == TEXT("sim_cache"))
				{
					OutState.Capabilities.PreviewStateKeys = { TEXT("absolute_time"), TEXT("time_seconds"), TEXT("normalized_time"), TEXT("cache_frame"), TEXT("sim_cache_frame"), TEXT("frame"), TEXT("tick_delta_seconds"), TEXT("pause"), TEXT("reset"), TEXT("restore_interactive_preview") };
				}
				else
				{
					OutState.Capabilities.PreviewStateKeys = { TEXT("target_frame"), TEXT("frame"), TEXT("pause_at_frame"), TEXT("tick_delta_seconds"), TEXT("advance_mode"), TEXT("pause"), TEXT("reset"), TEXT("restore_interactive_preview") };
				}
				OutState.VisualOptions = MakeNiagaraVisualOptionsJson(*Client, ViewportWidget);
				OutState.PreviewState = MakeNiagaraPreviewStateJson(*NiagaraSystem, PreviewComponent, Viewport.Target.ViewportRole, SimCache);
				return true;
			}

			virtual bool WriteCamera(const FResolvedViewport& Viewport, const FViewportCamera& Camera, FString& OutError) const override
			{
				if (!Viewport.ViewportClient)
				{
					OutError = TEXT("invalid_niagara_viewport");
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
				UNiagaraSimCache* SimCache = nullptr;
				UNiagaraSystem* NiagaraSystem = ResolveNiagaraSystemFromViewportAsset(Viewport, SimCache, OutError);
				if (!NiagaraSystem)
				{
					return false;
				}
				TSharedPtr<FNiagaraSystemViewModel> SystemViewModel;
				UNiagaraComponent* PreviewComponent = nullptr;
				TSharedPtr<SEditorViewport> ViewportWidget;
				FEditorViewportClient* Client = Viewport.ViewportClient;
				if (!ResolveNiagaraRuntime(NiagaraSystem, false, SystemViewModel, PreviewComponent, ViewportWidget, Client, OutError))
				{
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
					FocusBox = PreviewComponent ? PreviewComponent->Bounds.GetBox() : FBox(ForceInit);
				}
				else if (Mode == TEXT("custom_bounds"))
				{
					FVector Min;
					FVector Max;
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
					OutError = TEXT("unsupported_niagara_focus_mode");
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
				if (!FocusBox.IsValid && PreviewComponent)
				{
					FocusBox = FBox::BuildAABB(PreviewComponent->GetComponentLocation(), FVector(50.0f));
				}
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
				OutData->SetStringField(TEXT("space"), TEXT("niagara_preview_world"));
				OutData->SetStringField(TEXT("bounds_space"), TEXT("niagara_preview_world"));
				OutData->SetBoolField(TEXT("instant"), bInstant);
				TSharedPtr<FJsonObject> BoundsJson = MakeShared<FJsonObject>();
				BoundsJson->SetObjectField(TEXT("min"), MakeVectorJson(FocusBox.Min));
				BoundsJson->SetObjectField(TEXT("max"), MakeVectorJson(FocusBox.Max));
				BoundsJson->SetObjectField(TEXT("center"), MakeVectorJson(FocusBox.GetCenter()));
				BoundsJson->SetObjectField(TEXT("extent"), MakeVectorJson(FocusBox.GetExtent()));
				OutData->SetObjectField(TEXT("bounds"), BoundsJson);
				return true;
			}

			virtual bool ApplyVisualOptions(const FResolvedViewport& Viewport, const TSharedPtr<FJsonObject>& Options, TSharedPtr<FJsonObject>& OutData, FString& OutError) const override
			{
				UNiagaraSimCache* SimCache = nullptr;
				UNiagaraSystem* NiagaraSystem = ResolveNiagaraSystemFromViewportAsset(Viewport, SimCache, OutError);
				if (!NiagaraSystem)
				{
					return false;
				}
				TSharedPtr<FNiagaraSystemViewModel> SystemViewModel;
				UNiagaraComponent* PreviewComponent = nullptr;
				TSharedPtr<SEditorViewport> ViewportWidget;
				FEditorViewportClient* Client = Viewport.ViewportClient;
				if (!ResolveNiagaraRuntime(NiagaraSystem, false, SystemViewModel, PreviewComponent, ViewportWidget, Client, OutError))
				{
					return false;
				}

				TArray<FString> Unsupported;
				FString ViewModeText;
				if (TryGetStringField(Options, TEXT("view_mode"), ViewModeText))
				{
					EViewModeIndex ViewMode;
					if (!TryParseViewMode(ViewModeText, ViewMode))
					{
						Unsupported.Add(TEXT("view_mode"));
					}
					else
					{
						Client->SetViewMode(ViewMode);
					}
				}
				const FNiagaraEditorCommands& Commands = FNiagaraEditorCommands::Get();
				const TSharedPtr<FUICommandList> CommandList = ViewportWidget.IsValid() ? ViewportWidget->GetCommandList() : nullptr;
				bool bValue = false;
				if (TryReadBoolOption(Options, TEXT("grid"), bValue))
				{
					if (!TrySetCommandBool(CommandList, NiagaraCommand(Commands.TogglePreviewGrid), bValue))
					{
						Client->EngineShowFlags.SetGrid(bValue);
					}
				}
				if (TryReadBoolOption(Options, TEXT("bounds"), bValue))
				{
					Client->EngineShowFlags.SetBounds(bValue);
				}
				ApplyNiagaraCommandBoolOption(Options, TEXT("origin_axis"), NiagaraCommand(Commands.ToggleOriginAxis), CommandList, Unsupported);
				ApplyNiagaraCommandBoolOption(Options, TEXT("particle_counts"), NiagaraCommand(Commands.ToggleParticleCounts), CommandList, Unsupported);
				ApplyNiagaraCommandBoolOption(Options, TEXT("background"), NiagaraCommand(Commands.TogglePreviewBackground), CommandList, Unsupported);
				ApplyNiagaraCommandBoolOption(Options, TEXT("instruction_counts"), NiagaraCommand(Commands.ToggleInstructionCounts), CommandList, Unsupported);
				if (TryGetBoolField(Options, TEXT("realtime"), bValue))
				{
					Client->SetRealtime(bValue);
				}
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Options->Values)
				{
					const FString Key = NormalizeToken(Pair.Key);
					if (Key != TEXT("overlays") && Key != TEXT("view_mode") && Key != TEXT("view_mode_value") && Key != TEXT("grid") && Key != TEXT("bounds") && Key != TEXT("origin_axis") && Key != TEXT("particle_counts") && Key != TEXT("background") && Key != TEXT("instruction_counts") && Key != TEXT("realtime") && Key != TEXT("ignore_unsupported"))
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
						if (Key != TEXT("grid") && Key != TEXT("bounds") && Key != TEXT("origin_axis") && Key != TEXT("particle_counts") && Key != TEXT("background") && Key != TEXT("instruction_counts"))
						{
							Unsupported.AddUnique(Pair.Key);
						}
					}
				}
				Client->Invalidate();
				if (Client->Viewport)
				{
					Client->Viewport->Invalidate();
				}
				OutData = MakeNiagaraVisualOptionsJson(*Client, ViewportWidget);
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

				UNiagaraSimCache* SimCache = nullptr;
				UNiagaraSystem* NiagaraSystem = ResolveNiagaraSystemFromViewportAsset(Viewport, SimCache, OutError);
				if (!NiagaraSystem)
				{
					return false;
				}
				TSharedPtr<FNiagaraSystemViewModel> SystemViewModel;
				UNiagaraComponent* PreviewComponent = nullptr;
				TSharedPtr<SEditorViewport> ViewportWidget;
				FEditorViewportClient* Client = Viewport.ViewportClient;
				if (!ResolveNiagaraRuntime(NiagaraSystem, false, SystemViewModel, PreviewComponent, ViewportWidget, Client, OutError))
				{
					return false;
				}

				bool bRestoreInteractive = false;
				if (TryGetBoolField(PreviewState, TEXT("restore_interactive_preview"), bRestoreInteractive) && bRestoreInteractive)
				{
					SetPreviewStateToken(PreviewComponent, FString());
					PreviewComponent->SetAgeUpdateMode(ENiagaraAgeUpdateMode::TickDeltaTime);
					PreviewComponent->SetCanRenderWhileSeeking(false);
					PreviewComponent->SetAllowScalability(false);
					PreviewComponent->SetRenderingEnabled(true);
					PreviewComponent->SetPaused(false);
					PreviewComponent->Activate(true);
					if (SimCache)
					{
						PreviewComponent->SetSimCache(SimCache, true);
					}
					PreviewComponent->MarkRenderStateDirty();
					FlushNiagaraPreviewWorld(PreviewComponent);
					OutData->SetBoolField(TEXT("applied"), true);
					OutData->SetStringField(TEXT("mode"), TEXT("restore_interactive_preview"));
					OutData->SetObjectField(TEXT("preview_state"), MakeNiagaraPreviewStateJson(*NiagaraSystem, PreviewComponent, Viewport.Target.ViewportRole, SimCache));
					return true;
				}

				double TickDeltaNumber = 1.0 / 60.0;
				if (Viewport.Target.ViewportRole == TEXT("baker"))
				{
					TickDeltaNumber = 1.0 / static_cast<double>(GetBakerFramesPerSecond(NiagaraSystem));
				}
				TryGetNumberField(PreviewState, TEXT("tick_delta_seconds"), TickDeltaNumber);
				TryGetNumberField(PreviewState, TEXT("preview_tick_delta_seconds"), TickDeltaNumber);
				const float TickDeltaSeconds = FMath::Clamp(static_cast<float>(TickDeltaNumber), 1.0f / 240.0f, 1.0f / 5.0f);

				FNiagaraTemporalSeek TemporalSeek;
				if (TryResolveNiagaraTemporalSeek(PreviewState, Viewport.Target.ViewportRole, NiagaraSystem, SimCache, TickDeltaSeconds, TemporalSeek))
				{
					bool bPauseAfterSeek = true;
					TryGetBoolField(PreviewState, TEXT("pause_after_advance"), bPauseAfterSeek);
					TryGetBoolField(PreviewState, TEXT("pause"), bPauseAfterSeek);
					TryGetBoolField(PreviewState, TEXT("paused"), bPauseAfterSeek);
					bool bResetPreview = true;
					TryGetBoolField(PreviewState, TEXT("reset"), bResetPreview);
					TryGetBoolField(PreviewState, TEXT("reset_preview"), bResetPreview);

					if (NiagaraSystem->HasOutstandingCompilationRequests(true))
					{
						NiagaraSystem->WaitForCompilationComplete(true, false);
						NiagaraSystem->PollForCompilationComplete(true);
					}
					NiagaraSystem->CacheFromCompiledData();

					SetPreviewStateToken(PreviewComponent, FString());
					ConfigurePreviewComponentForFrame(PreviewComponent, NiagaraSystem, TickDeltaSeconds);
					if (SimCache)
					{
						PreviewComponent->SetSimCache(SimCache, true);
					}
					if (bResetPreview)
					{
						PreviewComponent->DeactivateImmediate();
						FlushNiagaraPreviewWorld(PreviewComponent);
						PreviewComponent->SetDesiredAge(0.0f);
						PreviewComponent->ReinitializeSystem();
						PreviewComponent = SystemViewModel->GetPreviewComponent();
						if (!PreviewComponent)
						{
							OutError = TEXT("niagara_preview_component_missing_after_reset");
							return false;
						}
						ConfigurePreviewComponentForFrame(PreviewComponent, NiagaraSystem, TickDeltaSeconds);
						if (SimCache)
						{
							PreviewComponent->SetSimCache(SimCache, true);
						}
						FlushNiagaraPreviewWorld(PreviewComponent);
					}
					PreviewComponent->SetAgeUpdateMode(ENiagaraAgeUpdateMode::DesiredAge);
					PreviewComponent->SetCanRenderWhileSeeking(true);
					PreviewComponent->SetPaused(false);
					PreviewComponent->Activate(false);
					PreviewComponent->SetDesiredAge(TemporalSeek.AbsoluteTimeSeconds);
					PreviewComponent->SeekToDesiredAge(TemporalSeek.AbsoluteTimeSeconds);
					PreviewComponent->TickComponent(0.0f, ELevelTick::LEVELTICK_All, nullptr);
					if (bPauseAfterSeek)
					{
						PreviewComponent->SetPaused(true);
					}
					PreviewComponent->MarkRenderStateDirty();
					FlushNiagaraPreviewWorld(PreviewComponent);

					const FString SourcePath = SimCache ? SimCache->GetOutermost()->GetName() : NiagaraSystem->GetOutermost()->GetName();
					const FString Token = MakeAbsolutePreviewStateToken(NiagaraSystem, PreviewComponent, Viewport.Target.ViewportRole, TemporalSeek.Mode, TemporalSeek.AbsoluteTimeSeconds, SourcePath);
					SetPreviewStateToken(PreviewComponent, Token);
					OutData->SetBoolField(TEXT("applied"), true);
					OutData->SetStringField(TEXT("mode"), TEXT("seek_absolute_time"));
					OutData->SetStringField(TEXT("time_source"), TemporalSeek.Mode);
					OutData->SetNumberField(TEXT("absolute_time_seconds"), TemporalSeek.AbsoluteTimeSeconds);
					OutData->SetNumberField(TEXT("tick_delta_seconds"), TickDeltaSeconds);
					OutData->SetBoolField(TEXT("pause_after_advance"), bPauseAfterSeek);
					if (TemporalSeek.SourceFrame != INDEX_NONE)
					{
						OutData->SetNumberField(TEXT("source_frame"), TemporalSeek.SourceFrame);
					}
					if (TemporalSeek.NormalizedTime >= 0.0f)
					{
						OutData->SetNumberField(TEXT("normalized_time"), TemporalSeek.NormalizedTime);
					}
					OutData->SetStringField(TEXT("preview_state_token"), Token);
					OutData->SetObjectField(TEXT("preview_state"), MakeNiagaraPreviewStateJson(*NiagaraSystem, PreviewComponent, Viewport.Target.ViewportRole, SimCache));
					return true;
				}

				double TargetFrameNumber = -1.0;
				const bool bHasTargetFrame =
					TryGetNumberField(PreviewState, TEXT("target_frame"), TargetFrameNumber) ||
					TryGetNumberField(PreviewState, TEXT("frame"), TargetFrameNumber) ||
					TryGetNumberField(PreviewState, TEXT("pause_at_frame"), TargetFrameNumber);

				bool bResetPreview = bHasTargetFrame;
				TryGetBoolField(PreviewState, TEXT("reset"), bResetPreview);
				TryGetBoolField(PreviewState, TEXT("reset_preview"), bResetPreview);

				if (bHasTargetFrame)
				{
					const int32 TargetFrame = FMath::Clamp(FMath::RoundToInt(TargetFrameNumber), 0, 14400);
					bool bPauseAfterAdvance = true;
					TryGetBoolField(PreviewState, TEXT("pause_after_advance"), bPauseAfterAdvance);
					TryGetBoolField(PreviewState, TEXT("pause"), bPauseAfterAdvance);
					TryGetBoolField(PreviewState, TEXT("paused"), bPauseAfterAdvance);
					FString AdvanceMode = TEXT("tick_component");
					TryGetStringField(PreviewState, TEXT("advance_mode"), AdvanceMode);
					AdvanceMode = NormalizeToken(AdvanceMode);
					const bool bUseAdvanceSimulation = AdvanceMode == TEXT("advance") || AdvanceMode == TEXT("advance_simulation");
					const bool bUseTickComponent = AdvanceMode.IsEmpty() || AdvanceMode == TEXT("tick") || AdvanceMode == TEXT("tick_component") || AdvanceMode == TEXT("tick_delta") || AdvanceMode == TEXT("tick_delta_time");
					if (!bUseAdvanceSimulation && !bUseTickComponent)
					{
						OutError = TEXT("unsupported_continuous_advance_mode");
						OutData->SetStringField(TEXT("advance_mode"), AdvanceMode);
						return false;
					}
					if (NiagaraSystem->HasOutstandingCompilationRequests(true))
					{
						NiagaraSystem->WaitForCompilationComplete(true, false);
						NiagaraSystem->PollForCompilationComplete(true);
					}
					NiagaraSystem->CacheFromCompiledData();

					SetPreviewStateToken(PreviewComponent, FString());
					ConfigurePreviewComponentForFrame(PreviewComponent, NiagaraSystem, TickDeltaSeconds);
					if (SimCache)
					{
						PreviewComponent->SetSimCache(SimCache, true);
					}
					if (bResetPreview)
					{
						PreviewComponent->DeactivateImmediate();
						FlushNiagaraPreviewWorld(PreviewComponent);
						PreviewComponent->SetDesiredAge(0.0f);
						PreviewComponent->ReinitializeSystem();
						PreviewComponent = SystemViewModel->GetPreviewComponent();
						if (!PreviewComponent)
						{
							OutError = TEXT("niagara_preview_component_missing_after_reset");
							return false;
						}
						ConfigurePreviewComponentForFrame(PreviewComponent, NiagaraSystem, TickDeltaSeconds);
						if (SimCache)
						{
							PreviewComponent->SetSimCache(SimCache, true);
						}
						FlushNiagaraPreviewWorld(PreviewComponent);
					}
					PreviewComponent->SetPaused(false);
					PreviewComponent->Activate(false);
					FlushNiagaraPreviewWorld(PreviewComponent);
					for (int32 FrameIndex = 0; FrameIndex < TargetFrame; ++FrameIndex)
					{
						if (bUseAdvanceSimulation)
						{
							PreviewComponent->AdvanceSimulation(1, TickDeltaSeconds);
						}
						else
						{
							PreviewComponent->TickComponent(TickDeltaSeconds, ELevelTick::LEVELTICK_All, nullptr);
						}
						FlushNiagaraPreviewWorld(PreviewComponent);
					}
					if (bPauseAfterAdvance)
					{
						PreviewComponent->SetPaused(true);
					}
					PreviewComponent->MarkRenderStateDirty();
					const FString EffectiveAdvanceMode = bUseAdvanceSimulation ? TEXT("advance_simulation") : TEXT("tick_component");
					const FString Token = MakePreviewStateToken(NiagaraSystem, PreviewComponent, TargetFrame, TickDeltaSeconds, EffectiveAdvanceMode);
					SetPreviewStateToken(PreviewComponent, Token);
					OutData->SetBoolField(TEXT("applied"), true);
					OutData->SetStringField(TEXT("mode"), TEXT("pause_at_frame"));
					OutData->SetNumberField(TEXT("target_frame"), TargetFrame);
					OutData->SetNumberField(TEXT("tick_delta_seconds"), TickDeltaSeconds);
					OutData->SetStringField(TEXT("advance_mode"), EffectiveAdvanceMode);
					OutData->SetBoolField(TEXT("pause_after_advance"), bPauseAfterAdvance);
					OutData->SetStringField(TEXT("preview_state_token"), Token);
					OutData->SetObjectField(TEXT("preview_state"), MakeNiagaraPreviewStateJson(*NiagaraSystem, PreviewComponent, Viewport.Target.ViewportRole, SimCache));
					return true;
				}

				if (bResetPreview)
				{
					SetPreviewStateToken(PreviewComponent, FString());
					ConfigurePreviewComponentForFrame(PreviewComponent, NiagaraSystem, TickDeltaSeconds);
					PreviewComponent->DeactivateImmediate();
					FlushNiagaraPreviewWorld(PreviewComponent);
					PreviewComponent->SetDesiredAge(0.0f);
					PreviewComponent->ReinitializeSystem();
					PreviewComponent = SystemViewModel->GetPreviewComponent();
					if (!PreviewComponent)
					{
						OutError = TEXT("niagara_preview_component_missing_after_reset");
						return false;
					}
					ConfigurePreviewComponentForFrame(PreviewComponent, NiagaraSystem, TickDeltaSeconds);
					if (SimCache)
					{
						PreviewComponent->SetSimCache(SimCache, true);
					}
					PreviewComponent->Activate(false);
					FlushNiagaraPreviewWorld(PreviewComponent);
					OutData->SetBoolField(TEXT("applied"), true);
					OutData->SetStringField(TEXT("mode"), TEXT("reset"));
					OutData->SetObjectField(TEXT("preview_state"), MakeNiagaraPreviewStateJson(*NiagaraSystem, PreviewComponent, Viewport.Target.ViewportRole, SimCache));
					return true;
				}

				bool bPause = false;
				const bool bHasPause = TryGetBoolField(PreviewState, TEXT("pause"), bPause) || TryGetBoolField(PreviewState, TEXT("paused"), bPause);
				if (bHasPause)
				{
					PreviewComponent->SetPaused(bPause);
					PreviewComponent->MarkRenderStateDirty();
					FlushNiagaraPreviewWorld(PreviewComponent);
					OutData->SetBoolField(TEXT("applied"), true);
					OutData->SetStringField(TEXT("mode"), TEXT("pause"));
					OutData->SetBoolField(TEXT("component_paused"), PreviewComponent->IsPaused());
					OutData->SetObjectField(TEXT("preview_state"), MakeNiagaraPreviewStateJson(*NiagaraSystem, PreviewComponent, Viewport.Target.ViewportRole, SimCache));
					return true;
				}

				OutError = TEXT("unsupported_niagara_preview_state");
				return false;
			}

			virtual bool CaptureScreenshot(const FResolvedViewport& Viewport, const TSharedPtr<FJsonObject>& Params, FViewportScreenshotResult& OutResult, FString& OutError) const override
			{
				if (!Viewport.ViewportClient)
				{
					OutError = TEXT("invalid_niagara_viewport");
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
				if (Viewport.Target.ViewportRole == TEXT("baker"))
				{
					OutResult.CaptureBackend = TEXT("niagara_baker_editor_backbuffer");
				}
				else if (Viewport.Target.ViewportRole == TEXT("sim_cache"))
				{
					OutResult.CaptureBackend = TEXT("niagara_sim_cache_editor_backbuffer");
				}
				else
				{
					OutResult.CaptureBackend = TEXT("niagara_editor_backbuffer");
				}
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
				TSharedPtr<FJsonObject> Applied;
				if (!ApplyVisualOptions(Viewport, Snapshot.State.VisualOptions, Applied, OutError))
				{
					return false;
				}
				UNiagaraSimCache* SimCache = nullptr;
				UNiagaraSystem* NiagaraSystem = ResolveNiagaraSystemFromViewportAsset(Viewport, SimCache, OutError);
				if (!NiagaraSystem)
				{
					return true;
				}
				TSharedPtr<FNiagaraSystemViewModel> SystemViewModel;
				UNiagaraComponent* PreviewComponent = nullptr;
				TSharedPtr<SEditorViewport> ViewportWidget;
				FEditorViewportClient* Client = Viewport.ViewportClient;
				if (!ResolveNiagaraRuntime(NiagaraSystem, false, SystemViewModel, PreviewComponent, ViewportWidget, Client, OutError))
				{
					return false;
				}
				SetPreviewStateToken(PreviewComponent, FString());
				PreviewComponent->SetPaused(false);
				PreviewComponent->SetAgeUpdateMode(ENiagaraAgeUpdateMode::TickDeltaTime);
				PreviewComponent->SetCanRenderWhileSeeking(false);
				if (SimCache)
				{
					PreviewComponent->SetSimCache(SimCache, true);
				}
				PreviewComponent->Activate(true);
				FlushNiagaraPreviewWorld(PreviewComponent);
				return true;
			}

		private:
			static bool ResolveNiagaraRuntime(
				UNiagaraSystem* NiagaraSystem,
				const bool bOpenIfNeeded,
				TSharedPtr<FNiagaraSystemViewModel>& OutSystemViewModel,
				UNiagaraComponent*& OutPreviewComponent,
				TSharedPtr<SEditorViewport>& OutViewportWidget,
				FEditorViewportClient*& InOutClient,
				FString& OutError)
			{
				OutSystemViewModel.Reset();
				OutPreviewComponent = nullptr;
				if (!NiagaraSystem)
				{
					OutError = TEXT("invalid_niagara_system_asset");
					return false;
				}
				OutViewportWidget = ResolveNiagaraViewportWidget(NiagaraSystem, bOpenIfNeeded, OutSystemViewModel, OutPreviewComponent, OutError);
				if (!OutViewportWidget.IsValid())
				{
					return false;
				}
				TSharedPtr<FEditorViewportClient> Client = OutViewportWidget->GetViewportClient();
				if (!Client.IsValid())
				{
					OutError = TEXT("niagara_viewport_client_not_found");
					return false;
				}
				InOutClient = Client.Get();
				return true;
			}

			static IAssetEditorInstance* GetNiagaraAssetEditor(UObject* Asset, const bool bOpenIfNeeded, FString& OutError)
			{
				if (!Asset)
				{
					OutError = TEXT("asset_not_found");
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
					OutError = bOpenIfNeeded ? TEXT("niagara_editor_open_failed") : TEXT("niagara_editor_not_opened");
					return nullptr;
				}
				return AssetEditor;
			}

			static TSharedPtr<SWindow> ResolveEditorWindow(IAssetEditorInstance* AssetEditor, FString& OutError)
			{
				if (!AssetEditor)
				{
					OutError = TEXT("niagara_editor_not_opened");
					return nullptr;
				}
				const TSharedPtr<FTabManager> TabManager = AssetEditor->GetAssociatedTabManager();
				if (!TabManager.IsValid())
				{
					OutError = TEXT("niagara_tab_manager_not_found");
					return nullptr;
				}
				const TSharedPtr<SDockTab> OwnerTab = TabManager->GetOwnerTab();
				if (!OwnerTab.IsValid())
				{
					OutError = TEXT("niagara_owner_tab_not_found");
					return nullptr;
				}
				OwnerTab->ActivateInParent(ETabActivationCause::SetDirectly);
				OwnerTab->SlatePrepass();
				if (FSlateApplication::IsInitialized())
				{
					FSlateApplication::Get().Tick();
				}
				const TSharedPtr<SWindow> Window = OwnerTab->GetParentWindow();
				if (!Window.IsValid())
				{
					OutError = TEXT("niagara_window_not_found");
					return nullptr;
				}
				return Window;
			}

			static TSharedPtr<SEditorViewport> ResolveNiagaraViewportWidget(
				UNiagaraSystem* NiagaraSystem,
				const bool bOpenIfNeeded,
				TSharedPtr<FNiagaraSystemViewModel>& OutSystemViewModel,
				UNiagaraComponent*& OutPreviewComponent,
				FString& OutError)
			{
				OutSystemViewModel.Reset();
				OutPreviewComponent = nullptr;
				if (!NiagaraSystem)
				{
					OutError = TEXT("invalid_niagara_system_asset");
					return nullptr;
				}
				IAssetEditorInstance* AssetEditor = GetNiagaraAssetEditor(NiagaraSystem, bOpenIfNeeded, OutError);
				if (!AssetEditor)
				{
					return nullptr;
				}
				FNiagaraEditorModule* NiagaraEditorModule = FModuleManager::GetModulePtr<FNiagaraEditorModule>(TEXT("NiagaraEditor"));
				if (!NiagaraEditorModule)
				{
					OutError = TEXT("niagara_editor_module_not_loaded");
					return nullptr;
				}
				OutSystemViewModel = NiagaraEditorModule->GetExistingViewModelForSystem(NiagaraSystem);
				if (!OutSystemViewModel.IsValid())
				{
					OutError = TEXT("niagara_system_view_model_not_found");
					return nullptr;
				}
				OutPreviewComponent = OutSystemViewModel->GetPreviewComponent();
				if (!OutPreviewComponent)
				{
					OutError = TEXT("niagara_preview_component_not_found");
					return nullptr;
				}
				const TSharedPtr<SWindow> Window = ResolveEditorWindow(AssetEditor, OutError);
				if (!Window.IsValid())
				{
					return nullptr;
				}
				TArray<TSharedRef<SWidget>> CandidateWidgets;
				FindWidgetsByTypeRecursive(Window.ToSharedRef(), FName(TEXT("SNiagaraSystemViewport")), CandidateWidgets);
				if (CandidateWidgets.Num() == 0)
				{
					TSharedPtr<SWidget> Fallback;
					if (FindWidgetByTypeRecursive(Window.ToSharedRef(), FName(TEXT("SEditorViewport")), Fallback) && Fallback.IsValid())
					{
						CandidateWidgets.Add(Fallback.ToSharedRef());
					}
				}
				TSharedPtr<SWidget> Widget = ChooseLargestWidget(CandidateWidgets);
				if (!Widget.IsValid())
				{
					OutError = TEXT("niagara_viewport_widget_not_found");
					return nullptr;
				}
				TSharedPtr<SEditorViewport> ViewportWidget = StaticCastSharedPtr<SEditorViewport>(Widget);
				if (!ViewportWidget.IsValid() || !ViewportWidget->GetViewportClient().IsValid())
				{
					OutError = TEXT("niagara_editor_viewport_invalid");
					return nullptr;
				}
				return ViewportWidget;
			}

			static void ApplyNiagaraCommandBoolOption(
				const TSharedPtr<FJsonObject>& Options,
				const FString& Key,
				const TSharedPtr<FUICommandInfo>& Command,
				const TSharedPtr<FUICommandList>& CommandList,
				TArray<FString>& Unsupported)
			{
				bool bValue = false;
				if (TryReadBoolOption(Options, Key, bValue) && !TrySetCommandBool(CommandList, Command, bValue))
				{
					Unsupported.AddUnique(Key);
				}
			}
		};
	}

	TSharedRef<IViewportAdapter> MakeNiagaraViewportAdapter()
	{
		return MakeShared<FNiagaraViewportAdapter>();
	}
}
