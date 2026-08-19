// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_Level.h"
#include "UeAgentHttpServer_LevelOps.h"

#include "Commands/EditorViewport/UeAgentEditorViewportSequenceFrameOptions.h"
#include "Utilities/UeAgentJsonDiagnostics.h"
#include "Module/UeAgentInterfaceLogger.h"
#include "UeAgentInterfaceSettings.h"

#include "Animation/AnimSequenceBase.h"
#include "Async/Async.h"
#include "Async/TaskGraphInterfaces.h"
#include "ActorFactories/ActorFactory.h"
#include "Builders/CubeBuilder.h"
#include "Components/ActorComponent.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/DynamicMeshComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SphereComponent.h"
#include "Components/SplineMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/Ticker.h"
#include "EditorActorFolders.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "EditorViewportClient.h"
#include "Engine/CollisionProfile.h"
#include "Engine/DirectionalLight.h"
#include "Engine/Level.h"
#include "Engine/OverlapResult.h"
#include "Engine/PlanarReflection.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/TargetPoint.h"
#include "Engine/TextureRenderTarget2D.h"
#include "EngineUtils.h"
#include "BufferVisualizationData.h"
#include "FileHelpers.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/ActorPrimitiveColorHandler.h"
#include "GeometryCacheComponent.h"
#include "GPUSkinCacheVisualizationData.h"
#include "GroomVisualizationData.h"
#include "ImageUtils.h"
#include "LevelEditorViewport.h"
#include "LumenVisualizationData.h"
#include "MeshDescription.h"
#include "HAL/PlatformProcess.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "NaniteVisualizationData.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "NavigationPath.h"
#include "NavigationSystem.h"
#include "NiagaraComponent.h"
#include "PhysicsEngine/AggregateGeom.h"
#include "PhysicsEngine/BodySetup.h"
#include "RayTracingVisualizationData.h"
#include "RenderingThread.h"
#include "Selection.h"
#include "ScopedTransaction.h"
#include "SceneView.h"
#include "StaticMeshAttributes.h"
#include "SubstrateVisualizationData.h"
#include "Animation/MorphTarget.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "VirtualShadowMapVisualizationData.h"
#include <atomic>

namespace UeAgentLevelOps
{
	static bool TryGetString(const TSharedPtr<FJsonObject>& Obj, const FString& Key, FString& OutValue)
	{
		return Obj.IsValid() && Obj->TryGetStringField(Key, OutValue);
	}

	static bool TryGetNumber(const TSharedPtr<FJsonObject>& Obj, const FString& Key, double& OutValue)
	{
		return Obj.IsValid() && Obj->TryGetNumberField(Key, OutValue);
	}

	static FString NormalizeLongPackagePath(const FString& InPath)
	{
		FString Path = InPath;
		Path.TrimStartAndEndInline();
		Path.ReplaceInline(TEXT("\\"), TEXT("/"));
		if (Path.EndsWith(FPackageName::GetMapPackageExtension()))
		{
			FString PackagePath;
			if (FPackageName::TryConvertFilenameToLongPackageName(Path, PackagePath))
			{
				Path = PackagePath;
			}
		}
		int32 DotIndex = INDEX_NONE;
		if (Path.FindChar(TEXT('.'), DotIndex) && DotIndex > 0)
		{
			Path = Path.Left(DotIndex);
		}
		return Path;
	}

	bool TryGetLevelPackagePath(const TSharedPtr<FJsonObject>& Params, FString& OutPackagePath)
	{
		if (!Params.IsValid())
		{
			return false;
		}
		FString Path;
		if (!Params->TryGetStringField(TEXT("asset_path"), Path)
			&& !Params->TryGetStringField(TEXT("level_path"), Path)
			&& !Params->TryGetStringField(TEXT("map_path"), Path)
			&& !Params->TryGetStringField(TEXT("package_path"), Path))
		{
			return false;
		}
		OutPackagePath = NormalizeLongPackagePath(Path);
		return !OutPackagePath.IsEmpty();
	}

	bool TryLevelPackageToFilename(const FString& PackagePath, FString& OutFilename, FString& OutError)
	{
		if (!FPackageName::IsValidLongPackageName(PackagePath))
		{
			OutError = TEXT("invalid_level_package_path");
			return false;
		}
		if (!FPackageName::TryConvertLongPackageNameToFilename(PackagePath, OutFilename, FPackageName::GetMapPackageExtension()))
		{
			OutError = TEXT("level_filename_conversion_failed");
			return false;
		}
		return true;
	}

	static TSharedPtr<FJsonObject> MakeVectorJson(const FVector& Value)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("x"), Value.X);
		Obj->SetNumberField(TEXT("y"), Value.Y);
		Obj->SetNumberField(TEXT("z"), Value.Z);
		return Obj;
	}

	static TSharedPtr<FJsonObject> MakeRotatorJson(const FRotator& Value)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("pitch"), Value.Pitch);
		Obj->SetNumberField(TEXT("yaw"), Value.Yaw);
		Obj->SetNumberField(TEXT("roll"), Value.Roll);
		return Obj;
	}

	static FString ViewportTypeToString(ELevelViewportType ViewportType)
	{
		switch (ViewportType)
		{
		case LVT_Perspective: return TEXT("Perspective");
		case LVT_OrthoXY: return TEXT("OrthoXY");
		case LVT_OrthoXZ: return TEXT("OrthoXZ");
		case LVT_OrthoYZ: return TEXT("OrthoYZ");
		default: return FString::Printf(TEXT("Unknown(%d)"), (int32)ViewportType);
		}
	}

	static FString WidgetModeToString(UE::Widget::EWidgetMode WidgetMode)
	{
		switch (WidgetMode)
		{
		case UE::Widget::WM_None: return TEXT("None");
		case UE::Widget::WM_Translate: return TEXT("Translate");
		case UE::Widget::WM_Rotate: return TEXT("Rotate");
		case UE::Widget::WM_Scale: return TEXT("Scale");
		default: return FString::Printf(TEXT("Unknown(%d)"), (int32)WidgetMode);
		}
	}

	static bool TryParseWidgetMode(const FString& InMode, UE::Widget::EWidgetMode& OutMode)
	{
		FString Mode = InMode.TrimStartAndEnd().ToLower();
		if (Mode.IsEmpty() || Mode == TEXT("keep") || Mode == TEXT("translate_if_none"))
		{
			return false;
		}
		if (Mode == TEXT("none"))
		{
			OutMode = UE::Widget::WM_None;
			return true;
		}
		if (Mode == TEXT("translate") || Mode == TEXT("translation") || Mode == TEXT("move"))
		{
			OutMode = UE::Widget::WM_Translate;
			return true;
		}
		if (Mode == TEXT("rotate") || Mode == TEXT("rotation"))
		{
			OutMode = UE::Widget::WM_Rotate;
			return true;
		}
		if (Mode == TEXT("scale"))
		{
			OutMode = UE::Widget::WM_Scale;
			return true;
		}
		return false;
	}

	FResolvedViewportViewMode::FResolvedViewportViewMode()
		: ViewMode(VMI_Lit)
		, VisualizationFamily(EViewportVisualizationFamily::None)
		, VisualizationMode(NAME_None)
		, bFinalLitComposition(true)
	{
	}

	FViewportViewModeState::FViewportViewModeState()
		: ViewMode(VMI_Lit)
		, BufferMode(NAME_None)
		, NaniteMode(NAME_None)
		, LumenMode(NAME_None)
		, SubstrateMode(NAME_None)
		, GroomMode(NAME_None)
		, VirtualShadowMapMode(NAME_None)
		, VirtualTextureMode(NAME_None)
		, ActorColorationMode(NAME_None)
		, GPUSkinCacheMode(NAME_None)
		, RayTracingDebugMode(NAME_None)
	{
	}

	struct FSimpleViewportViewModeEntry
	{
		const TCHAR* Key;
		const TCHAR* Category;
		const TCHAR* Label;
		EViewModeIndex ViewMode;
	};

	static FString NormalizeViewportModeToken(const FString& InText)
	{
		FString Out;
		Out.Reserve(InText.Len());
		for (TCHAR Ch : InText)
		{
			if (FChar::IsAlnum(Ch))
			{
				Out.AppendChar(FChar::ToLower(Ch));
			}
		}
		return Out;
	}

	static FString MakeModeKeySegment(const FString& InText)
	{
		FString Out;
		Out.Reserve(InText.Len() + 8);
		TCHAR PrevOut = 0;
		TCHAR PrevSource = 0;
		for (int32 Index = 0; Index < InText.Len(); ++Index)
		{
			const TCHAR Ch = InText[Index];
			if (FChar::IsAlnum(Ch))
			{
				if (FChar::IsUpper(Ch)
					&& Out.Len() > 0
					&& PrevOut != TEXT('_')
					&& (FChar::IsLower(PrevSource) || FChar::IsDigit(PrevSource)))
				{
					Out.AppendChar(TEXT('_'));
				}
				Out.AppendChar(FChar::ToLower(Ch));
				PrevOut = Out[Out.Len() - 1];
			}
			else if (Out.Len() > 0 && PrevOut != TEXT('_'))
			{
				Out.AppendChar(TEXT('_'));
				PrevOut = TEXT('_');
			}
			PrevSource = Ch;
		}
		while (Out.EndsWith(TEXT("_")))
		{
			Out.LeftChopInline(1);
		}
		return Out;
	}

	FString ViewModeIndexToStableName(EViewModeIndex ViewMode)
	{
		switch (ViewMode)
		{
		case VMI_BrushWireframe: return TEXT("brush_wireframe");
		case VMI_Wireframe: return TEXT("csg_wireframe");
		case VMI_Unlit: return TEXT("unlit");
		case VMI_Lit: return TEXT("lit");
		case VMI_Lit_DetailLighting: return TEXT("detail_lighting");
		case VMI_LightingOnly: return TEXT("lighting_only");
		case VMI_LightComplexity: return TEXT("light_complexity");
		case VMI_ShaderComplexity: return TEXT("shader_complexity");
		case VMI_LightmapDensity: return TEXT("lightmap_density");
		case VMI_LitLightmapDensity: return TEXT("lit_lightmap_density");
		case VMI_ReflectionOverride: return TEXT("reflections");
		case VMI_VisualizeBuffer: return TEXT("buffer_visualization");
		case VMI_StationaryLightOverlap: return TEXT("stationary_light_overlap");
		case VMI_CollisionPawn: return TEXT("player_collision");
		case VMI_CollisionVisibility: return TEXT("visibility_collision");
		case VMI_LODColoration: return TEXT("lod_coloration");
		case VMI_QuadOverdraw: return TEXT("quad_overdraw");
		case VMI_PrimitiveDistanceAccuracy: return TEXT("primitive_distance_accuracy");
		case VMI_MeshUVDensityAccuracy: return TEXT("mesh_uv_density_accuracy");
		case VMI_ShaderComplexityWithQuadOverdraw: return TEXT("shader_complexity_with_quad_overdraw");
		case VMI_HLODColoration: return TEXT("hlod_coloration");
		case VMI_GroupLODColoration: return TEXT("group_lod_coloration");
		case VMI_MaterialTextureScaleAccuracy: return TEXT("material_texture_scale_accuracy");
		case VMI_RequiredTextureResolution: return TEXT("required_texture_resolution");
		case VMI_PathTracing: return TEXT("path_tracing");
		case VMI_RayTracingDebug: return TEXT("ray_tracing_debug");
		case VMI_VisualizeNanite: return TEXT("nanite_visualization");
		case VMI_VisualizeVirtualTexture: return TEXT("virtual_texture_visualization");
		case VMI_VisualizeLumen: return TEXT("lumen_visualization");
		case VMI_VisualizeVirtualShadowMap: return TEXT("virtual_shadow_map_visualization");
		case VMI_VisualizeGPUSkinCache: return TEXT("gpu_skin_cache_visualization");
		case VMI_VisualizeSubstrate: return TEXT("substrate_visualization");
		case VMI_VisualizeGroom: return TEXT("groom_visualization");
		case VMI_LWCComplexity: return TEXT("lwc_complexity");
		case VMI_Lit_Wireframe: return TEXT("lit_wireframe");
		case VMI_VisualizeActorColoration: return TEXT("actor_coloration_visualization");
		default: return FString::Printf(TEXT("unknown_%d"), (int32)ViewMode);
		}
	}

	static FString ViewModeIndexToDisplayName(EViewModeIndex ViewMode)
	{
		switch (ViewMode)
		{
		case VMI_BrushWireframe: return TEXT("Wireframe");
		case VMI_Wireframe: return TEXT("CSG Wireframe");
		case VMI_Unlit: return TEXT("Unlit");
		case VMI_Lit: return TEXT("Lit");
		case VMI_Lit_DetailLighting: return TEXT("Detail Lighting");
		case VMI_LightingOnly: return TEXT("Lighting Only");
		case VMI_LightComplexity: return TEXT("Light Complexity");
		case VMI_ShaderComplexity: return TEXT("Shader Complexity");
		case VMI_LightmapDensity: return TEXT("Lightmap Density");
		case VMI_LitLightmapDensity: return TEXT("Lit Lightmap Density");
		case VMI_ReflectionOverride: return TEXT("Reflections");
		case VMI_VisualizeBuffer: return TEXT("Buffer Visualization");
		case VMI_StationaryLightOverlap: return TEXT("Stationary Light Overlap");
		case VMI_CollisionPawn: return TEXT("Player Collision");
		case VMI_CollisionVisibility: return TEXT("Visibility Collision");
		case VMI_LODColoration: return TEXT("Mesh LOD Coloration");
		case VMI_QuadOverdraw: return TEXT("Quad Overdraw");
		case VMI_PrimitiveDistanceAccuracy: return TEXT("Primitive Distance");
		case VMI_MeshUVDensityAccuracy: return TEXT("Mesh UV Density");
		case VMI_ShaderComplexityWithQuadOverdraw: return TEXT("Shader Complexity & Quads");
		case VMI_HLODColoration: return TEXT("Hierarchical LOD Coloration");
		case VMI_GroupLODColoration: return TEXT("Group LOD Coloration");
		case VMI_MaterialTextureScaleAccuracy: return TEXT("Material Texture Scales");
		case VMI_RequiredTextureResolution: return TEXT("Required Texture Resolution");
		case VMI_PathTracing: return TEXT("Path Tracing");
		case VMI_RayTracingDebug: return TEXT("Ray Tracing Debug");
		case VMI_VisualizeNanite: return TEXT("Nanite Visualization");
		case VMI_VisualizeVirtualTexture: return TEXT("Virtual Texture Visualization");
		case VMI_VisualizeLumen: return TEXT("Lumen Visualization");
		case VMI_VisualizeVirtualShadowMap: return TEXT("Virtual Shadow Map Visualization");
		case VMI_VisualizeGPUSkinCache: return TEXT("GPU Skin Cache Visualization");
		case VMI_VisualizeSubstrate: return TEXT("Substrate Visualization");
		case VMI_VisualizeGroom: return TEXT("Groom Visualization");
		case VMI_LWCComplexity: return TEXT("Material LWC Function Usage");
		case VMI_Lit_Wireframe: return TEXT("Lit Wireframe");
		case VMI_VisualizeActorColoration: return TEXT("Actor Coloration Visualization");
		default: return FString::Printf(TEXT("Unknown(%d)"), (int32)ViewMode);
		}
	}

	bool IsFinalLitCompositionViewMode(EViewModeIndex ViewMode)
	{
		return ViewMode == VMI_Lit;
	}

	static const TArray<FSimpleViewportViewModeEntry>& GetSimpleViewportViewModeEntries()
	{
		static const TArray<FSimpleViewportViewModeEntry> Entries = {
			{ TEXT("lit"), TEXT("view_mode"), TEXT("Lit"), VMI_Lit },
			{ TEXT("unlit"), TEXT("view_mode"), TEXT("Unlit"), VMI_Unlit },
			{ TEXT("wireframe"), TEXT("view_mode"), TEXT("Wireframe"), VMI_BrushWireframe },
			{ TEXT("brush_wireframe"), TEXT("view_mode"), TEXT("Wireframe"), VMI_BrushWireframe },
			{ TEXT("csg_wireframe"), TEXT("view_mode"), TEXT("CSG Wireframe"), VMI_Wireframe },
			{ TEXT("lit_wireframe"), TEXT("view_mode"), TEXT("Lit Wireframe"), VMI_Lit_Wireframe },
			{ TEXT("detail_lighting"), TEXT("view_mode"), TEXT("Detail Lighting"), VMI_Lit_DetailLighting },
			{ TEXT("lighting_only"), TEXT("view_mode"), TEXT("Lighting Only"), VMI_LightingOnly },
			{ TEXT("reflections"), TEXT("view_mode"), TEXT("Reflections"), VMI_ReflectionOverride },
			{ TEXT("reflection_override"), TEXT("view_mode"), TEXT("Reflections"), VMI_ReflectionOverride },
			{ TEXT("player_collision"), TEXT("view_mode"), TEXT("Player Collision"), VMI_CollisionPawn },
			{ TEXT("pawn_collision"), TEXT("view_mode"), TEXT("Player Collision"), VMI_CollisionPawn },
			{ TEXT("visibility_collision"), TEXT("view_mode"), TEXT("Visibility Collision"), VMI_CollisionVisibility },
			{ TEXT("light_complexity"), TEXT("optimization"), TEXT("Light Complexity"), VMI_LightComplexity },
			{ TEXT("lightmap_density"), TEXT("optimization"), TEXT("Lightmap Density"), VMI_LightmapDensity },
			{ TEXT("lit_lightmap_density"), TEXT("optimization"), TEXT("Lit Lightmap Density"), VMI_LitLightmapDensity },
			{ TEXT("stationary_light_overlap"), TEXT("optimization"), TEXT("Stationary Light Overlap"), VMI_StationaryLightOverlap },
			{ TEXT("shader_complexity"), TEXT("optimization"), TEXT("Shader Complexity"), VMI_ShaderComplexity },
			{ TEXT("shader_complexity_and_quads"), TEXT("optimization"), TEXT("Shader Complexity & Quads"), VMI_ShaderComplexityWithQuadOverdraw },
			{ TEXT("shader_complexity_with_quad_overdraw"), TEXT("optimization"), TEXT("Shader Complexity & Quads"), VMI_ShaderComplexityWithQuadOverdraw },
			{ TEXT("quad_overdraw"), TEXT("optimization"), TEXT("Quad Overdraw"), VMI_QuadOverdraw },
			{ TEXT("lwc_complexity"), TEXT("optimization"), TEXT("Material LWC Function Usage"), VMI_LWCComplexity },
			{ TEXT("primitive_distance"), TEXT("texture_streaming_accuracy"), TEXT("Primitive Distance"), VMI_PrimitiveDistanceAccuracy },
			{ TEXT("primitive_distance_accuracy"), TEXT("texture_streaming_accuracy"), TEXT("Primitive Distance"), VMI_PrimitiveDistanceAccuracy },
			{ TEXT("mesh_uv_density"), TEXT("texture_streaming_accuracy"), TEXT("Mesh UV Density"), VMI_MeshUVDensityAccuracy },
			{ TEXT("mesh_uv_density_accuracy"), TEXT("texture_streaming_accuracy"), TEXT("Mesh UV Density"), VMI_MeshUVDensityAccuracy },
			{ TEXT("material_texture_scales"), TEXT("texture_streaming_accuracy"), TEXT("Material Texture Scales"), VMI_MaterialTextureScaleAccuracy },
			{ TEXT("material_texture_scale_accuracy"), TEXT("texture_streaming_accuracy"), TEXT("Material Texture Scales"), VMI_MaterialTextureScaleAccuracy },
			{ TEXT("required_texture_resolution"), TEXT("texture_streaming_accuracy"), TEXT("Required Texture Resolution"), VMI_RequiredTextureResolution },
			{ TEXT("lod_coloration"), TEXT("lod"), TEXT("Mesh LOD Coloration"), VMI_LODColoration },
			{ TEXT("hlod_coloration"), TEXT("lod"), TEXT("Hierarchical LOD Coloration"), VMI_HLODColoration },
			{ TEXT("path_tracing"), TEXT("ray_tracing"), TEXT("Path Tracing"), VMI_PathTracing },
			{ TEXT("ray_tracing_debug"), TEXT("ray_tracing"), TEXT("Ray Tracing Debug"), VMI_RayTracingDebug },
		};
		return Entries;
	}

	static FString VisualizationFamilyToString(EViewportVisualizationFamily Family)
	{
		switch (Family)
		{
		case EViewportVisualizationFamily::Buffer: return TEXT("buffer");
		case EViewportVisualizationFamily::Nanite: return TEXT("nanite");
		case EViewportVisualizationFamily::Lumen: return TEXT("lumen");
		case EViewportVisualizationFamily::Substrate: return TEXT("substrate");
		case EViewportVisualizationFamily::Groom: return TEXT("groom");
		case EViewportVisualizationFamily::VirtualShadowMap: return TEXT("virtual_shadow_map");
		case EViewportVisualizationFamily::VirtualTexture: return TEXT("virtual_texture");
		case EViewportVisualizationFamily::ActorColoration: return TEXT("actor_coloration");
		case EViewportVisualizationFamily::GPUSkinCache: return TEXT("gpu_skin_cache");
		case EViewportVisualizationFamily::RayTracingDebug: return TEXT("ray_tracing_debug");
		default: return TEXT("");
		}
	}

	static EViewModeIndex VisualizationFamilyToViewMode(EViewportVisualizationFamily Family)
	{
		switch (Family)
		{
		case EViewportVisualizationFamily::Buffer: return VMI_VisualizeBuffer;
		case EViewportVisualizationFamily::Nanite: return VMI_VisualizeNanite;
		case EViewportVisualizationFamily::Lumen: return VMI_VisualizeLumen;
		case EViewportVisualizationFamily::Substrate: return VMI_VisualizeSubstrate;
		case EViewportVisualizationFamily::Groom: return VMI_VisualizeGroom;
		case EViewportVisualizationFamily::VirtualShadowMap: return VMI_VisualizeVirtualShadowMap;
		case EViewportVisualizationFamily::VirtualTexture: return VMI_VisualizeVirtualTexture;
		case EViewportVisualizationFamily::ActorColoration: return VMI_VisualizeActorColoration;
		case EViewportVisualizationFamily::GPUSkinCache: return VMI_VisualizeGPUSkinCache;
		case EViewportVisualizationFamily::RayTracingDebug: return VMI_RayTracingDebug;
		default: return VMI_Lit;
		}
	}

	static bool TryParseVisualizationFamily(const FString& InText, EViewportVisualizationFamily& OutFamily)
	{
		const FString Key = NormalizeViewportModeToken(InText);
		if (Key == TEXT("buffer") || Key == TEXT("buffervisualization") || Key == TEXT("visualizebuffer") || Key == TEXT("gbuffer"))
		{
			OutFamily = EViewportVisualizationFamily::Buffer;
			return true;
		}
		if (Key == TEXT("nanite") || Key == TEXT("nanitevisualization") || Key == TEXT("visualizenanite"))
		{
			OutFamily = EViewportVisualizationFamily::Nanite;
			return true;
		}
		if (Key == TEXT("lumen") || Key == TEXT("lumenvisualization") || Key == TEXT("visualizelumen"))
		{
			OutFamily = EViewportVisualizationFamily::Lumen;
			return true;
		}
		if (Key == TEXT("substrate") || Key == TEXT("substratevisualization") || Key == TEXT("visualizesubstrate"))
		{
			OutFamily = EViewportVisualizationFamily::Substrate;
			return true;
		}
		if (Key == TEXT("groom") || Key == TEXT("hair") || Key == TEXT("groomvisualization") || Key == TEXT("visualizegroom"))
		{
			OutFamily = EViewportVisualizationFamily::Groom;
			return true;
		}
		if (Key == TEXT("vsm") || Key == TEXT("virtualshadowmap") || Key == TEXT("virtualshadowmaps") || Key == TEXT("virtualshadowmapvisualization"))
		{
			OutFamily = EViewportVisualizationFamily::VirtualShadowMap;
			return true;
		}
		if (Key == TEXT("vt") || Key == TEXT("virtualtexture") || Key == TEXT("virtualtextures") || Key == TEXT("virtualtexturevisualization"))
		{
			OutFamily = EViewportVisualizationFamily::VirtualTexture;
			return true;
		}
		if (Key == TEXT("actorcoloration") || Key == TEXT("actorcolor") || Key == TEXT("actorcolorationvisualization"))
		{
			OutFamily = EViewportVisualizationFamily::ActorColoration;
			return true;
		}
		if (Key == TEXT("gpuskincache") || Key == TEXT("skincache") || Key == TEXT("gpuskincachevisualization"))
		{
			OutFamily = EViewportVisualizationFamily::GPUSkinCache;
			return true;
		}
		if (Key == TEXT("raytracingdebug") || Key == TEXT("rtdebug") || Key == TEXT("raytracing"))
		{
			OutFamily = EViewportVisualizationFamily::RayTracingDebug;
			return true;
		}
		return false;
	}

	static bool GetDefaultVisualizationMode(EViewportVisualizationFamily Family, FName& OutModeName, FString& OutLabel)
	{
		switch (Family)
		{
		case EViewportVisualizationFamily::Buffer:
			OutModeName = NAME_None;
			OutLabel = TEXT("Overview");
			return true;
		case EViewportVisualizationFamily::Nanite:
			OutModeName = TEXT("Overview");
			OutLabel = TEXT("Overview");
			return true;
		case EViewportVisualizationFamily::Lumen:
			OutModeName = TEXT("Overview");
			OutLabel = TEXT("Overview");
			return true;
		case EViewportVisualizationFamily::Substrate:
			OutModeName = TEXT("MaterialProperties");
			OutLabel = TEXT("Material Properties");
			return true;
		case EViewportVisualizationFamily::Groom:
			OutModeName = TEXT("None");
			OutLabel = TEXT("None");
			return true;
		case EViewportVisualizationFamily::VirtualShadowMap:
			OutModeName = TEXT("mask");
			OutLabel = TEXT("Shadow Mask");
			return true;
		case EViewportVisualizationFamily::VirtualTexture:
			OutModeName = TEXT("pending");
			OutLabel = TEXT("Pending Mips");
			return true;
		case EViewportVisualizationFamily::ActorColoration:
			OutModeName = FActorPrimitiveColorHandler::Get().GetActivePrimitiveColorHandler();
			OutLabel = FActorPrimitiveColorHandler::Get().GetActivePrimitiveColorHandlerDisplayName().ToString();
			return true;
		case EViewportVisualizationFamily::GPUSkinCache:
			OutModeName = TEXT("Overview");
			OutLabel = TEXT("Overview");
			return true;
		case EViewportVisualizationFamily::RayTracingDebug:
			OutModeName = TEXT("Barycentrics");
			OutLabel = TEXT("Barycentrics");
			return true;
		default:
			return false;
		}
	}

	template <typename TModeMapType>
	static bool TryResolveVisualizationRecordMode(const TModeMapType& ModeMap, const FString& InMode, FName& OutModeName, FString& OutLabel)
	{
		const FString Wanted = NormalizeViewportModeToken(InMode);
		if (Wanted.IsEmpty())
		{
			return false;
		}
		for (auto It = ModeMap.CreateConstIterator(); It; ++It)
		{
			const auto& Record = It.Value();
			const FString ModeName = Record.ModeName.ToString();
			const FString ModeString = Record.ModeString;
			const FString ModeLabel = Record.ModeText.ToString();
			if (NormalizeViewportModeToken(ModeName) == Wanted
				|| NormalizeViewportModeToken(ModeString) == Wanted
				|| NormalizeViewportModeToken(ModeLabel) == Wanted)
			{
				OutModeName = Record.ModeName;
				OutLabel = ModeLabel.IsEmpty() ? ModeName : ModeLabel;
				return true;
			}
		}
		return false;
	}

	static bool TryResolveBufferVisualizationMode(const FString& InMode, FName& OutModeName, FString& OutLabel)
	{
		const FString Wanted = NormalizeViewportModeToken(InMode);
		if (Wanted.IsEmpty() || Wanted == TEXT("overview") || Wanted == TEXT("default") || Wanted == TEXT("none"))
		{
			OutModeName = NAME_None;
			OutLabel = TEXT("Overview");
			return true;
		}

		struct FBufferModeResolver
		{
			FString Wanted;
			FName ModeName = NAME_None;
			FString Label;
			bool bFound = false;

			void ProcessValue(const FString& Name, UMaterialInterface* Material, const FText& DisplayName)
			{
				if (bFound)
				{
					return;
				}
				const FString MaterialName = Material ? Material->GetName() : FString();
				if (NormalizeViewportModeToken(Name) == Wanted
					|| NormalizeViewportModeToken(DisplayName.ToString()) == Wanted
					|| NormalizeViewportModeToken(MaterialName) == Wanted)
				{
					ModeName = FName(*Name);
					Label = DisplayName.ToString().IsEmpty() ? Name : DisplayName.ToString();
					bFound = true;
				}
			}
		};

		FBufferVisualizationData& Data = GetBufferVisualizationData();
		FBufferModeResolver Resolver;
		Resolver.Wanted = Wanted;
		Data.IterateOverAvailableMaterials(Resolver);
		if (Resolver.bFound)
		{
			OutModeName = Resolver.ModeName;
			OutLabel = Resolver.Label;
			return true;
		}
		return false;
	}

	static bool TryResolveActorColorationMode(const FString& InMode, FName& OutModeName, FString& OutLabel)
	{
		const FString Wanted = NormalizeViewportModeToken(InMode);
		if (Wanted.IsEmpty() || Wanted == TEXT("current"))
		{
			OutModeName = FActorPrimitiveColorHandler::Get().GetActivePrimitiveColorHandler();
			OutLabel = FActorPrimitiveColorHandler::Get().GetActivePrimitiveColorHandlerDisplayName().ToString();
			return true;
		}
		if (Wanted == TEXT("none") || Wanted == TEXT("off"))
		{
			OutModeName = NAME_None;
			OutLabel = TEXT("None");
			return true;
		}

		TArray<FActorPrimitiveColorHandler::FPrimitiveColorHandler> Handlers;
		FActorPrimitiveColorHandler::Get().GetRegisteredPrimitiveColorHandlers(Handlers);
		for (const FActorPrimitiveColorHandler::FPrimitiveColorHandler& Handler : Handlers)
		{
			if (!Handler.bAvailalbleInEditor)
			{
				continue;
			}
			if (NormalizeViewportModeToken(Handler.HandlerName.ToString()) == Wanted
				|| NormalizeViewportModeToken(Handler.HandlerText.ToString()) == Wanted)
			{
				OutModeName = Handler.HandlerName;
				OutLabel = Handler.HandlerText.ToString();
				return true;
			}
		}
		return false;
	}

	static bool TryResolveVirtualTextureVisualizationMode(const FString& InMode, FName& OutModeName, FString& OutLabel)
	{
		const FString Wanted = NormalizeViewportModeToken(InMode);
		if (Wanted == TEXT("pending") || Wanted == TEXT("pendingmips"))
		{
			OutModeName = TEXT("pending");
			OutLabel = TEXT("Pending Mips");
			return true;
		}
		if (Wanted == TEXT("count") || Wanted == TEXT("stackcount"))
		{
			OutModeName = TEXT("count");
			OutLabel = TEXT("Stack Count");
			return true;
		}
		return false;
	}

	static bool TryResolveVisualizationModeInFamily(EViewportVisualizationFamily Family, const FString& InMode, FName& OutModeName, FString& OutLabel)
	{
		if (InMode.TrimStartAndEnd().IsEmpty())
		{
			return GetDefaultVisualizationMode(Family, OutModeName, OutLabel);
		}

		switch (Family)
		{
		case EViewportVisualizationFamily::Buffer:
			return TryResolveBufferVisualizationMode(InMode, OutModeName, OutLabel);
		case EViewportVisualizationFamily::Nanite:
		{
			FNaniteVisualizationData& Data = GetNaniteVisualizationData();
			return TryResolveVisualizationRecordMode(Data.GetModeMap(), InMode, OutModeName, OutLabel);
		}
		case EViewportVisualizationFamily::Lumen:
		{
			FLumenVisualizationData& Data = GetLumenVisualizationData();
			return TryResolveVisualizationRecordMode(Data.GetModeMap(), InMode, OutModeName, OutLabel);
		}
		case EViewportVisualizationFamily::Substrate:
		{
			FSubstrateVisualizationData& Data = GetSubstrateVisualizationData();
			return TryResolveVisualizationRecordMode(Data.GetModeMap(), InMode, OutModeName, OutLabel);
		}
		case EViewportVisualizationFamily::Groom:
		{
			FGroomVisualizationData& Data = GetGroomVisualizationData();
			return TryResolveVisualizationRecordMode(Data.GetModeMap(), InMode, OutModeName, OutLabel);
		}
		case EViewportVisualizationFamily::VirtualShadowMap:
		{
			FVirtualShadowMapVisualizationData& Data = GetVirtualShadowMapVisualizationData();
			return TryResolveVisualizationRecordMode(Data.GetModeMap(), InMode, OutModeName, OutLabel);
		}
		case EViewportVisualizationFamily::VirtualTexture:
			return TryResolveVirtualTextureVisualizationMode(InMode, OutModeName, OutLabel);
		case EViewportVisualizationFamily::ActorColoration:
			return TryResolveActorColorationMode(InMode, OutModeName, OutLabel);
		case EViewportVisualizationFamily::GPUSkinCache:
		{
			FGPUSkinCacheVisualizationData& Data = GetGPUSkinCacheVisualizationData();
			return TryResolveVisualizationRecordMode(Data.GetModeMap(), InMode, OutModeName, OutLabel);
		}
		case EViewportVisualizationFamily::RayTracingDebug:
		{
			FRayTracingVisualizationData& Data = GetRayTracingVisualizationData();
			return TryResolveVisualizationRecordMode(Data.GetModeMap(), InMode, OutModeName, OutLabel);
		}
		default:
			return false;
		}
	}

	static FResolvedViewportViewMode MakeResolvedVisualizationViewMode(
		EViewportVisualizationFamily Family,
		const FName& ModeName,
		const FString& Label)
	{
		FResolvedViewportViewMode Resolved;
		Resolved.VisualizationFamily = Family;
		Resolved.VisualizationMode = ModeName;
		Resolved.ViewMode = VisualizationFamilyToViewMode(Family);
		Resolved.Category = VisualizationFamilyToString(Family);
		Resolved.Label = Label.IsEmpty() ? ModeName.ToString() : Label;
		Resolved.Key = MakeModeKeySegment(Resolved.Category + TEXT("_") + (ModeName.IsNone() ? TEXT("overview") : ModeName.ToString()));
		Resolved.bFinalLitComposition = false;
		return Resolved;
	}

	static bool TryResolveViewportViewModeString(const FString& InMode, FResolvedViewportViewMode& OutResolved, FString& OutError)
	{
		const FString ModeTrim = InMode.TrimStartAndEnd();
		const FString Wanted = NormalizeViewportModeToken(ModeTrim);
		if (Wanted.IsEmpty())
		{
			OutError = TEXT("missing_view_mode");
			return false;
		}

		for (const FSimpleViewportViewModeEntry& Entry : GetSimpleViewportViewModeEntries())
		{
			if (NormalizeViewportModeToken(Entry.Key) == Wanted
				|| NormalizeViewportModeToken(Entry.Label) == Wanted
				|| NormalizeViewportModeToken(ViewModeIndexToStableName(Entry.ViewMode)) == Wanted)
			{
				OutResolved.Key = Entry.Key;
				OutResolved.Category = Entry.Category;
				OutResolved.Label = Entry.Label;
				OutResolved.ViewMode = Entry.ViewMode;
				OutResolved.VisualizationFamily = EViewportVisualizationFamily::None;
				OutResolved.VisualizationMode = NAME_None;
				OutResolved.bFinalLitComposition = IsFinalLitCompositionViewMode(Entry.ViewMode);
				return true;
			}
		}

		EViewportVisualizationFamily Family = EViewportVisualizationFamily::None;
		if (TryParseVisualizationFamily(ModeTrim, Family))
		{
			FName ModeName = NAME_None;
			FString Label;
			if (GetDefaultVisualizationMode(Family, ModeName, Label))
			{
				OutResolved = MakeResolvedVisualizationViewMode(Family, ModeName, Label);
				return true;
			}
		}

		FString FamilyPart;
		FString ModePart;
		if (ModeTrim.Split(TEXT(":"), &FamilyPart, &ModePart)
			|| ModeTrim.Split(TEXT("."), &FamilyPart, &ModePart)
			|| ModeTrim.Split(TEXT("/"), &FamilyPart, &ModePart))
		{
			if (TryParseVisualizationFamily(FamilyPart, Family))
			{
				FName ModeName = NAME_None;
				FString Label;
				if (TryResolveVisualizationModeInFamily(Family, ModePart, ModeName, Label))
				{
					OutResolved = MakeResolvedVisualizationViewMode(Family, ModeName, Label);
					return true;
				}
				OutError = FString::Printf(TEXT("unsupported_visualization_mode:%s"), *ModePart);
				return false;
			}
		}

		const EViewportVisualizationFamily Families[] = {
			EViewportVisualizationFamily::Buffer,
			EViewportVisualizationFamily::Nanite,
			EViewportVisualizationFamily::Lumen,
			EViewportVisualizationFamily::Substrate,
			EViewportVisualizationFamily::Groom,
			EViewportVisualizationFamily::VirtualShadowMap,
			EViewportVisualizationFamily::VirtualTexture,
			EViewportVisualizationFamily::ActorColoration,
			EViewportVisualizationFamily::GPUSkinCache,
			EViewportVisualizationFamily::RayTracingDebug,
		};

		int32 MatchCount = 0;
		FResolvedViewportViewMode Matched;
		for (EViewportVisualizationFamily CandidateFamily : Families)
		{
			const FString FamilyKey = VisualizationFamilyToString(CandidateFamily);
			const FString FamilyNormalized = NormalizeViewportModeToken(FamilyKey);
			const bool bHasFamilyPrefix = Wanted.StartsWith(FamilyNormalized) && Wanted.Len() > FamilyNormalized.Len();

			TArray<FString> CandidateModeTexts;
			CandidateModeTexts.Add(ModeTrim);
			if (bHasFamilyPrefix)
			{
				CandidateModeTexts.Add(Wanted.Mid(FamilyNormalized.Len()));
			}

			for (const FString& CandidateModeText : CandidateModeTexts)
			{
				FName ModeName = NAME_None;
				FString Label;
				if (TryResolveVisualizationModeInFamily(CandidateFamily, CandidateModeText, ModeName, Label))
				{
					Matched = MakeResolvedVisualizationViewMode(CandidateFamily, ModeName, Label);
					++MatchCount;
					break;
				}
			}
		}

		if (MatchCount == 1)
		{
			OutResolved = Matched;
			return true;
		}
		if (MatchCount > 1)
		{
			OutError = TEXT("ambiguous_visualization_mode");
			return false;
		}

		OutError = TEXT("unsupported_view_mode");
		return false;
	}

	bool TryResolveRequestedViewportViewMode(
		const TSharedPtr<FJsonObject>& Params,
		bool& bOutHasViewMode,
		FResolvedViewportViewMode& OutResolved,
		FString& OutError)
	{
		bOutHasViewMode = false;
		if (!Params.IsValid())
		{
			return true;
		}

		FString FamilyText;
		if (Params->TryGetStringField(TEXT("visualization_family"), FamilyText)
			|| Params->TryGetStringField(TEXT("visualizer"), FamilyText)
			|| Params->TryGetStringField(TEXT("view_mode_family"), FamilyText))
		{
			EViewportVisualizationFamily Family = EViewportVisualizationFamily::None;
			if (!TryParseVisualizationFamily(FamilyText, Family))
			{
				OutError = TEXT("unsupported_visualization_family");
				return false;
			}

			FString ModeText;
			Params->TryGetStringField(TEXT("visualization_mode"), ModeText)
				|| Params->TryGetStringField(TEXT("visualizer_mode"), ModeText)
				|| Params->TryGetStringField(TEXT("debug_mode"), ModeText);

			FName ModeName = NAME_None;
			FString Label;
			if (!TryResolveVisualizationModeInFamily(Family, ModeText, ModeName, Label))
			{
				OutError = TEXT("unsupported_visualization_mode");
				return false;
			}

			OutResolved = MakeResolvedVisualizationViewMode(Family, ModeName, Label);
			bOutHasViewMode = true;
			return true;
		}

		const TCHAR* Fields[] = {
			TEXT("view_mode"),
			TEXT("editor_view_mode"),
			TEXT("mode"),
			TEXT("mode_key"),
			TEXT("debug_view_mode"),
		};
		for (const TCHAR* Field : Fields)
		{
			FString ModeText;
			if (Params->TryGetStringField(Field, ModeText) && !ModeText.TrimStartAndEnd().IsEmpty())
			{
				if (!TryResolveViewportViewModeString(ModeText, OutResolved, OutError))
				{
					return false;
				}
				bOutHasViewMode = true;
				return true;
			}
		}

		return true;
	}

	FViewportViewModeState CaptureViewportViewModeState(FEditorViewportClient* ViewportClient)
	{
		FViewportViewModeState State;
		if (!ViewportClient)
		{
			return State;
		}

		State.ViewMode = ViewportClient->GetViewMode();
		State.BufferMode = ViewportClient->CurrentBufferVisualizationMode;
		State.NaniteMode = ViewportClient->CurrentNaniteVisualizationMode;
		State.LumenMode = ViewportClient->CurrentLumenVisualizationMode;
		State.SubstrateMode = ViewportClient->CurrentSubstrateVisualizationMode;
		State.GroomMode = ViewportClient->CurrentGroomVisualizationMode;
		State.VirtualShadowMapMode = ViewportClient->CurrentVirtualShadowMapVisualizationMode;
		State.VirtualTextureMode = ViewportClient->CurrentVirtualTextureVisualizationMode;
		State.ActorColorationMode = FActorPrimitiveColorHandler::Get().GetActivePrimitiveColorHandler();
		State.GPUSkinCacheMode = ViewportClient->CurrentGPUSkinCacheVisualizationMode;
		State.RayTracingDebugMode = ViewportClient->CurrentRayTracingDebugVisualizationMode;
		return State;
	}

	static void InvalidateViewportClient(FEditorViewportClient* ViewportClient)
	{
		if (!ViewportClient)
		{
			return;
		}
		ViewportClient->Invalidate();
		if (ViewportClient->Viewport)
		{
			ViewportClient->Viewport->Invalidate();
		}
	}

	void RestoreViewportViewModeState(FEditorViewportClient* ViewportClient, const FViewportViewModeState& State)
	{
		if (!ViewportClient)
		{
			return;
		}

		switch (State.ViewMode)
		{
		case VMI_VisualizeBuffer:
			ViewportClient->ChangeBufferVisualizationMode(State.BufferMode);
			break;
		case VMI_VisualizeNanite:
			ViewportClient->ChangeNaniteVisualizationMode(State.NaniteMode);
			break;
		case VMI_VisualizeLumen:
			ViewportClient->ChangeLumenVisualizationMode(State.LumenMode);
			break;
		case VMI_VisualizeSubstrate:
			ViewportClient->ChangeSubstrateVisualizationMode(State.SubstrateMode);
			break;
		case VMI_VisualizeGroom:
			ViewportClient->ChangeGroomVisualizationMode(State.GroomMode);
			break;
		case VMI_VisualizeVirtualShadowMap:
			ViewportClient->ChangeVirtualShadowMapVisualizationMode(State.VirtualShadowMapMode);
			break;
		case VMI_VisualizeVirtualTexture:
			ViewportClient->ChangeVirtualTextureVisualizationMode(State.VirtualTextureMode);
			break;
		case VMI_VisualizeActorColoration:
			ViewportClient->ChangeActorColorationVisualizationMode(State.ActorColorationMode);
			break;
		case VMI_VisualizeGPUSkinCache:
			ViewportClient->ChangeGPUSkinCacheVisualizationMode(State.GPUSkinCacheMode);
			break;
		case VMI_RayTracingDebug:
			ViewportClient->ChangeRayTracingDebugVisualizationMode(State.RayTracingDebugMode);
			break;
		default:
			ViewportClient->SetViewMode(State.ViewMode);
			break;
		}

		ViewportClient->CurrentBufferVisualizationMode = State.BufferMode;
		ViewportClient->CurrentNaniteVisualizationMode = State.NaniteMode;
		ViewportClient->CurrentLumenVisualizationMode = State.LumenMode;
		ViewportClient->CurrentSubstrateVisualizationMode = State.SubstrateMode;
		ViewportClient->CurrentGroomVisualizationMode = State.GroomMode;
		ViewportClient->CurrentVirtualShadowMapVisualizationMode = State.VirtualShadowMapMode;
		ViewportClient->CurrentVirtualTextureVisualizationMode = State.VirtualTextureMode;
		ViewportClient->CurrentGPUSkinCacheVisualizationMode = State.GPUSkinCacheMode;
		ViewportClient->CurrentRayTracingDebugVisualizationMode = State.RayTracingDebugMode;
		FActorPrimitiveColorHandler::Get().SetActivePrimitiveColorHandler(State.ActorColorationMode, GWorld);
		InvalidateViewportClient(ViewportClient);
	}

	void ApplyResolvedViewportViewMode(FEditorViewportClient* ViewportClient, const FResolvedViewportViewMode& Resolved)
	{
		if (!ViewportClient)
		{
			return;
		}

		switch (Resolved.VisualizationFamily)
		{
		case EViewportVisualizationFamily::Buffer:
			ViewportClient->ChangeBufferVisualizationMode(Resolved.VisualizationMode);
			break;
		case EViewportVisualizationFamily::Nanite:
			ViewportClient->ChangeNaniteVisualizationMode(Resolved.VisualizationMode);
			break;
		case EViewportVisualizationFamily::Lumen:
			ViewportClient->ChangeLumenVisualizationMode(Resolved.VisualizationMode);
			break;
		case EViewportVisualizationFamily::Substrate:
			ViewportClient->ChangeSubstrateVisualizationMode(Resolved.VisualizationMode);
			break;
		case EViewportVisualizationFamily::Groom:
			ViewportClient->ChangeGroomVisualizationMode(Resolved.VisualizationMode);
			break;
		case EViewportVisualizationFamily::VirtualShadowMap:
			ViewportClient->ChangeVirtualShadowMapVisualizationMode(Resolved.VisualizationMode);
			break;
		case EViewportVisualizationFamily::VirtualTexture:
			ViewportClient->ChangeVirtualTextureVisualizationMode(Resolved.VisualizationMode);
			break;
		case EViewportVisualizationFamily::ActorColoration:
			ViewportClient->ChangeActorColorationVisualizationMode(Resolved.VisualizationMode);
			break;
		case EViewportVisualizationFamily::GPUSkinCache:
			ViewportClient->ChangeGPUSkinCacheVisualizationMode(Resolved.VisualizationMode);
			break;
		case EViewportVisualizationFamily::RayTracingDebug:
			ViewportClient->ChangeRayTracingDebugVisualizationMode(Resolved.VisualizationMode);
			break;
		default:
			ViewportClient->SetViewMode(Resolved.ViewMode);
			break;
		}
		InvalidateViewportClient(ViewportClient);
	}

	TSharedPtr<FJsonObject> MakeResolvedViewportViewModeJson(const FResolvedViewportViewMode& Resolved)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("key"), Resolved.Key);
		Obj->SetStringField(TEXT("category"), Resolved.Category);
		Obj->SetStringField(TEXT("label"), Resolved.Label);
		Obj->SetStringField(TEXT("view_mode_name"), ViewModeIndexToStableName(Resolved.ViewMode));
		Obj->SetNumberField(TEXT("view_mode_value"), (int32)Resolved.ViewMode);
		Obj->SetBoolField(TEXT("final_lit_composition"), Resolved.bFinalLitComposition);
		if (Resolved.VisualizationFamily != EViewportVisualizationFamily::None)
		{
			Obj->SetStringField(TEXT("visualization_family"), VisualizationFamilyToString(Resolved.VisualizationFamily));
			Obj->SetStringField(TEXT("visualization_mode"), Resolved.VisualizationMode.ToString());
		}
		return Obj;
	}

	TSharedPtr<FJsonObject> MakeCurrentViewportViewModeJson(FEditorViewportClient* ViewportClient)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!ViewportClient)
		{
			Obj->SetBoolField(TEXT("valid"), false);
			return Obj;
		}

		const EViewModeIndex ViewMode = ViewportClient->GetViewMode();
		Obj->SetBoolField(TEXT("valid"), true);
		Obj->SetStringField(TEXT("view_mode_name"), ViewModeIndexToStableName(ViewMode));
		Obj->SetStringField(TEXT("view_mode_display_name"), ViewModeIndexToDisplayName(ViewMode));
		Obj->SetNumberField(TEXT("view_mode_value"), (int32)ViewMode);
		Obj->SetBoolField(TEXT("final_lit_composition"), IsFinalLitCompositionViewMode(ViewMode));

		FString ActiveVisualizationFamily;
		FName ActiveVisualizationMode = NAME_None;
		switch (ViewMode)
		{
		case VMI_VisualizeBuffer:
			ActiveVisualizationFamily = TEXT("buffer");
			ActiveVisualizationMode = ViewportClient->CurrentBufferVisualizationMode;
			break;
		case VMI_VisualizeNanite:
			ActiveVisualizationFamily = TEXT("nanite");
			ActiveVisualizationMode = ViewportClient->CurrentNaniteVisualizationMode;
			break;
		case VMI_VisualizeLumen:
			ActiveVisualizationFamily = TEXT("lumen");
			ActiveVisualizationMode = ViewportClient->CurrentLumenVisualizationMode;
			break;
		case VMI_VisualizeSubstrate:
			ActiveVisualizationFamily = TEXT("substrate");
			ActiveVisualizationMode = ViewportClient->CurrentSubstrateVisualizationMode;
			break;
		case VMI_VisualizeGroom:
			ActiveVisualizationFamily = TEXT("groom");
			ActiveVisualizationMode = ViewportClient->CurrentGroomVisualizationMode;
			break;
		case VMI_VisualizeVirtualShadowMap:
			ActiveVisualizationFamily = TEXT("virtual_shadow_map");
			ActiveVisualizationMode = ViewportClient->CurrentVirtualShadowMapVisualizationMode;
			break;
		case VMI_VisualizeVirtualTexture:
			ActiveVisualizationFamily = TEXT("virtual_texture");
			ActiveVisualizationMode = ViewportClient->CurrentVirtualTextureVisualizationMode;
			break;
		case VMI_VisualizeActorColoration:
			ActiveVisualizationFamily = TEXT("actor_coloration");
			ActiveVisualizationMode = FActorPrimitiveColorHandler::Get().GetActivePrimitiveColorHandler();
			break;
		case VMI_VisualizeGPUSkinCache:
			ActiveVisualizationFamily = TEXT("gpu_skin_cache");
			ActiveVisualizationMode = ViewportClient->CurrentGPUSkinCacheVisualizationMode;
			break;
		case VMI_RayTracingDebug:
			ActiveVisualizationFamily = TEXT("ray_tracing_debug");
			ActiveVisualizationMode = ViewportClient->CurrentRayTracingDebugVisualizationMode;
			break;
		default:
			break;
		}
		if (!ActiveVisualizationFamily.IsEmpty())
		{
			Obj->SetStringField(TEXT("visualization_family"), ActiveVisualizationFamily);
			Obj->SetStringField(TEXT("visualization_mode"), ActiveVisualizationMode.ToString());
		}

		TSharedPtr<FJsonObject> Visualization = MakeShared<FJsonObject>();
		Visualization->SetStringField(TEXT("buffer"), ViewportClient->CurrentBufferVisualizationMode.ToString());
		Visualization->SetStringField(TEXT("nanite"), ViewportClient->CurrentNaniteVisualizationMode.ToString());
		Visualization->SetStringField(TEXT("lumen"), ViewportClient->CurrentLumenVisualizationMode.ToString());
		Visualization->SetStringField(TEXT("substrate"), ViewportClient->CurrentSubstrateVisualizationMode.ToString());
		Visualization->SetStringField(TEXT("groom"), ViewportClient->CurrentGroomVisualizationMode.ToString());
		Visualization->SetStringField(TEXT("virtual_shadow_map"), ViewportClient->CurrentVirtualShadowMapVisualizationMode.ToString());
		Visualization->SetStringField(TEXT("virtual_texture"), ViewportClient->CurrentVirtualTextureVisualizationMode.ToString());
		Visualization->SetStringField(TEXT("actor_coloration"), FActorPrimitiveColorHandler::Get().GetActivePrimitiveColorHandler().ToString());
		Visualization->SetStringField(TEXT("gpu_skin_cache"), ViewportClient->CurrentGPUSkinCacheVisualizationMode.ToString());
		Visualization->SetStringField(TEXT("ray_tracing_debug"), ViewportClient->CurrentRayTracingDebugVisualizationMode.ToString());
		Obj->SetObjectField(TEXT("visualization_modes"), Visualization);
		return Obj;
	}

	static TSharedPtr<FJsonObject> MakeViewportViewModeCatalogEntry(
		const FString& Key,
		const FString& Category,
		const FString& Label,
		EViewModeIndex ViewMode,
		EViewportVisualizationFamily Family = EViewportVisualizationFamily::None,
		const FName& VisualizationMode = NAME_None)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("key"), Key);
		Obj->SetStringField(TEXT("category"), Category);
		Obj->SetStringField(TEXT("label"), Label);
		Obj->SetStringField(TEXT("view_mode_name"), ViewModeIndexToStableName(ViewMode));
		Obj->SetNumberField(TEXT("view_mode_value"), (int32)ViewMode);
		Obj->SetBoolField(TEXT("final_lit_composition"), IsFinalLitCompositionViewMode(ViewMode));
		if (Family != EViewportVisualizationFamily::None)
		{
			Obj->SetStringField(TEXT("visualization_family"), VisualizationFamilyToString(Family));
			Obj->SetStringField(TEXT("visualization_mode"), VisualizationMode.ToString());
		}
		return Obj;
	}

	template <typename TModeMapType>
	static void AppendVisualizationModeCatalogEntries(
		TArray<TSharedPtr<FJsonValue>>& OutModes,
		EViewportVisualizationFamily Family,
		const TModeMapType& ModeMap)
	{
		const FString FamilyKey = VisualizationFamilyToString(Family);
		for (auto It = ModeMap.CreateConstIterator(); It; ++It)
		{
			const auto& Record = It.Value();
			const FString ModeKey = Record.ModeString.IsEmpty() ? Record.ModeName.ToString() : Record.ModeString;
			const FString Label = Record.ModeText.ToString().IsEmpty() ? ModeKey : Record.ModeText.ToString();
			OutModes.Add(MakeShared<FJsonValueObject>(MakeViewportViewModeCatalogEntry(
				MakeModeKeySegment(FamilyKey + TEXT("_") + ModeKey),
				FamilyKey,
				Label,
				VisualizationFamilyToViewMode(Family),
				Family,
				Record.ModeName)));
		}
	}

	TSharedPtr<FJsonObject> MakeViewportViewModeCatalogJson()
	{
		TArray<TSharedPtr<FJsonValue>> Modes;

		for (const FSimpleViewportViewModeEntry& Entry : GetSimpleViewportViewModeEntries())
		{
			Modes.Add(MakeShared<FJsonValueObject>(MakeViewportViewModeCatalogEntry(
				Entry.Key,
				Entry.Category,
				Entry.Label,
				Entry.ViewMode)));
		}

		{
			Modes.Add(MakeShared<FJsonValueObject>(MakeViewportViewModeCatalogEntry(
				TEXT("buffer_overview"),
				TEXT("buffer"),
				TEXT("Overview"),
				VMI_VisualizeBuffer,
				EViewportVisualizationFamily::Buffer,
				NAME_None)));

			struct FBufferModeCatalogIterator
			{
				TArray<TSharedPtr<FJsonValue>>& Modes;
				void ProcessValue(const FString& Name, UMaterialInterface* Material, const FText& DisplayName)
				{
					Modes.Add(MakeShared<FJsonValueObject>(MakeViewportViewModeCatalogEntry(
						MakeModeKeySegment(TEXT("buffer_") + Name),
						TEXT("buffer"),
						DisplayName.ToString().IsEmpty() ? Name : DisplayName.ToString(),
						VMI_VisualizeBuffer,
						EViewportVisualizationFamily::Buffer,
						FName(*Name))));
				}
			};

			FBufferVisualizationData& Data = GetBufferVisualizationData();
			FBufferModeCatalogIterator Iterator{ Modes };
			Data.IterateOverAvailableMaterials(Iterator);
		}

		{
			FNaniteVisualizationData& Data = GetNaniteVisualizationData();
			AppendVisualizationModeCatalogEntries(Modes, EViewportVisualizationFamily::Nanite, Data.GetModeMap());
		}
		{
			FLumenVisualizationData& Data = GetLumenVisualizationData();
			AppendVisualizationModeCatalogEntries(Modes, EViewportVisualizationFamily::Lumen, Data.GetModeMap());
		}
		{
			FSubstrateVisualizationData& Data = GetSubstrateVisualizationData();
			AppendVisualizationModeCatalogEntries(Modes, EViewportVisualizationFamily::Substrate, Data.GetModeMap());
		}
		{
			FGroomVisualizationData& Data = GetGroomVisualizationData();
			AppendVisualizationModeCatalogEntries(Modes, EViewportVisualizationFamily::Groom, Data.GetModeMap());
		}
		{
			FVirtualShadowMapVisualizationData& Data = GetVirtualShadowMapVisualizationData();
			AppendVisualizationModeCatalogEntries(Modes, EViewportVisualizationFamily::VirtualShadowMap, Data.GetModeMap());
		}
		{
			Modes.Add(MakeShared<FJsonValueObject>(MakeViewportViewModeCatalogEntry(
				TEXT("virtual_texture_pending"),
				TEXT("virtual_texture"),
				TEXT("Pending Mips"),
				VMI_VisualizeVirtualTexture,
				EViewportVisualizationFamily::VirtualTexture,
				TEXT("pending"))));
			Modes.Add(MakeShared<FJsonValueObject>(MakeViewportViewModeCatalogEntry(
				TEXT("virtual_texture_count"),
				TEXT("virtual_texture"),
				TEXT("Stack Count"),
				VMI_VisualizeVirtualTexture,
				EViewportVisualizationFamily::VirtualTexture,
				TEXT("count"))));
		}
		{
			TArray<FActorPrimitiveColorHandler::FPrimitiveColorHandler> Handlers;
			FActorPrimitiveColorHandler::Get().GetRegisteredPrimitiveColorHandlers(Handlers);
			for (const FActorPrimitiveColorHandler::FPrimitiveColorHandler& Handler : Handlers)
			{
				if (!Handler.bAvailalbleInEditor)
				{
					continue;
				}
				Modes.Add(MakeShared<FJsonValueObject>(MakeViewportViewModeCatalogEntry(
					MakeModeKeySegment(TEXT("actor_coloration_") + Handler.HandlerName.ToString()),
					TEXT("actor_coloration"),
					Handler.HandlerText.ToString(),
					VMI_VisualizeActorColoration,
					EViewportVisualizationFamily::ActorColoration,
					Handler.HandlerName)));
			}
		}
		{
			FGPUSkinCacheVisualizationData& Data = GetGPUSkinCacheVisualizationData();
			AppendVisualizationModeCatalogEntries(Modes, EViewportVisualizationFamily::GPUSkinCache, Data.GetModeMap());
		}
		{
			FRayTracingVisualizationData& Data = GetRayTracingVisualizationData();
			AppendVisualizationModeCatalogEntries(Modes, EViewportVisualizationFamily::RayTracingDebug, Data.GetModeMap());
		}

		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("schema"), TEXT("uai.viewport_view_modes.v1"));
		Obj->SetStringField(TEXT("usage"), TEXT("apply a listed view mode, then use capture.image.execute with the exact viewport policy"));
		Obj->SetNumberField(TEXT("mode_count"), Modes.Num());
		Obj->SetArrayField(TEXT("modes"), Modes);
		return Obj;
	}

	static int32 GetLevelViewportClientIndex(FLevelEditorViewportClient* TargetClient)
	{
		if (!GEditor || !TargetClient)
		{
			return -1;
		}

		int32 Index = 0;
		for (FLevelEditorViewportClient* ViewportClient : GEditor->GetLevelViewportClients())
		{
			if (ViewportClient == TargetClient)
			{
				return Index;
			}
			Index++;
		}
		return -1;
	}

	TSharedPtr<FJsonObject> MakeViewportClientJson(FLevelEditorViewportClient* ViewportClient, const FString& SelectionReason)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("selection_reason"), SelectionReason);
		Obj->SetNumberField(TEXT("viewport_client_index"), GetLevelViewportClientIndex(ViewportClient));

		if (!ViewportClient)
		{
			Obj->SetBoolField(TEXT("valid"), false);
			return Obj;
		}

		FViewport* ActiveViewport = GEditor ? GEditor->GetActiveViewport() : nullptr;
		Obj->SetBoolField(TEXT("valid"), true);
		Obj->SetBoolField(TEXT("is_active_viewport"), ActiveViewport && ViewportClient->Viewport == ActiveViewport);
		Obj->SetBoolField(TEXT("is_current_level_editing_viewport"), ViewportClient == GCurrentLevelEditingViewportClient);
		Obj->SetStringField(TEXT("viewport_type"), ViewportTypeToString(ViewportClient->ViewportType));
		Obj->SetNumberField(TEXT("viewport_type_value"), (int32)ViewportClient->ViewportType);
		Obj->SetStringField(TEXT("view_mode"), FString::Printf(TEXT("%d"), (int32)ViewportClient->GetViewMode()));
		Obj->SetNumberField(TEXT("view_mode_value"), (int32)ViewportClient->GetViewMode());
		Obj->SetStringField(TEXT("view_mode_name"), ViewModeIndexToStableName(ViewportClient->GetViewMode()));
		Obj->SetStringField(TEXT("view_mode_display_name"), ViewModeIndexToDisplayName(ViewportClient->GetViewMode()));
		Obj->SetObjectField(TEXT("view_mode_state"), MakeCurrentViewportViewModeJson(ViewportClient));
		Obj->SetObjectField(TEXT("location"), MakeVectorJson(ViewportClient->GetViewLocation()));
		Obj->SetObjectField(TEXT("rotation"), MakeRotatorJson(ViewportClient->GetViewRotation()));
		Obj->SetNumberField(TEXT("fov"), ViewportClient->ViewFOV);
		Obj->SetBoolField(TEXT("realtime"), ViewportClient->IsRealtime());
		Obj->SetBoolField(TEXT("game_view"), ViewportClient->IsInGameView());
		Obj->SetStringField(TEXT("widget_mode"), WidgetModeToString(ViewportClient->GetWidgetMode()));
		Obj->SetNumberField(TEXT("widget_mode_value"), (int32)ViewportClient->GetWidgetMode());

		TSharedPtr<FJsonObject> ShowFlagsObj = MakeShared<FJsonObject>();
		ShowFlagsObj->SetBoolField(TEXT("mode_widgets"), ViewportClient->EngineShowFlags.ModeWidgets != 0);
		ShowFlagsObj->SetBoolField(TEXT("selection"), ViewportClient->EngineShowFlags.Selection != 0);
		ShowFlagsObj->SetBoolField(TEXT("pivot"), ViewportClient->EngineShowFlags.Pivot != 0);
		ShowFlagsObj->SetBoolField(TEXT("editor"), ViewportClient->EngineShowFlags.Editor != 0);
		ShowFlagsObj->SetBoolField(TEXT("game"), ViewportClient->EngineShowFlags.Game != 0);
		Obj->SetObjectField(TEXT("show_flags"), ShowFlagsObj);

		TSharedPtr<FJsonObject> SizeObj = MakeShared<FJsonObject>();
		if (ViewportClient->Viewport)
		{
			const FIntPoint Size = ViewportClient->Viewport->GetSizeXY();
			SizeObj->SetNumberField(TEXT("width"), Size.X);
			SizeObj->SetNumberField(TEXT("height"), Size.Y);
		}
		else
		{
			SizeObj->SetNumberField(TEXT("width"), 0);
			SizeObj->SetNumberField(TEXT("height"), 0);
		}
		Obj->SetObjectField(TEXT("size"), SizeObj);
		return Obj;
	}

	void RestoreEditorInteractionState(
		FLevelEditorViewportClient* ViewportClient,
		bool bGameView,
		bool bModeWidgets,
		bool bSelection,
		bool bPivot,
		const FString& WidgetModePolicy)
	{
		if (!ViewportClient)
		{
			return;
		}

		ViewportClient->SetGameView(bGameView);
		ViewportClient->EngineShowFlags.SetModeWidgets(bModeWidgets);
		ViewportClient->EngineShowFlags.SetSelection(bSelection);
		ViewportClient->EngineShowFlags.SetPivot(bPivot);

		UE::Widget::EWidgetMode ParsedMode = UE::Widget::WM_None;
		if (TryParseWidgetMode(WidgetModePolicy, ParsedMode))
		{
			ViewportClient->SetWidgetMode(ParsedMode);
		}
		else if (WidgetModePolicy.TrimStartAndEnd().ToLower() == TEXT("translate_if_none") && ViewportClient->GetWidgetMode() == UE::Widget::WM_None)
		{
			ViewportClient->SetWidgetMode(UE::Widget::WM_Translate);
		}

		ViewportClient->Invalidate();
		if (ViewportClient->Viewport)
		{
			ViewportClient->Viewport->Invalidate();
		}
	}

	FLevelEditorViewportClient* GetPreferredLevelViewportClient()
	{
		return GetPreferredLevelViewportClient(nullptr);
	}

	FLevelEditorViewportClient* GetPreferredLevelViewportClient(FString* OutSelectionReason)
	{
		auto SetReason = [OutSelectionReason](const TCHAR* Reason)
		{
			if (OutSelectionReason)
			{
				*OutSelectionReason = Reason;
			}
		};

		if (GEditor)
		{
			if (FViewport* ActiveViewport = GEditor->GetActiveViewport())
			{
				for (FLevelEditorViewportClient* ViewportClient : GEditor->GetLevelViewportClients())
				{
					if (ViewportClient && ViewportClient->Viewport == ActiveViewport)
					{
						SetReason(TEXT("active_viewport"));
						return ViewportClient;
					}
				}
			}
		}

		if (GCurrentLevelEditingViewportClient && GCurrentLevelEditingViewportClient->Viewport)
		{
			SetReason(TEXT("current_level_editing_viewport"));
			return GCurrentLevelEditingViewportClient;
		}

		if (GEditor)
		{
			FLevelEditorViewportClient* FirstValid = nullptr;

			FLevelEditorViewportClient* BestPerspectiveByArea = nullptr;
			int64 BestPerspectiveArea = -1;

			FLevelEditorViewportClient* BestAnyByArea = nullptr;
			int64 BestAnyArea = -1;
			for (FLevelEditorViewportClient* ViewportClient : GEditor->GetLevelViewportClients())
			{
				if (!ViewportClient || !ViewportClient->Viewport)
				{
					continue;
				}
				if (!FirstValid)
				{
					FirstValid = ViewportClient;
				}

				const FIntPoint Size = ViewportClient->Viewport->GetSizeXY();
				const int64 Area = (int64)Size.X * (int64)Size.Y;
				if (Area > BestAnyArea)
				{
					BestAnyArea = Area;
					BestAnyByArea = ViewportClient;
				}
				if (ViewportClient->ViewportType == LVT_Perspective && Area > BestPerspectiveArea)
				{
					BestPerspectiveArea = Area;
					BestPerspectiveByArea = ViewportClient;
				}
			}
			if (BestPerspectiveByArea)
			{
				SetReason(TEXT("largest_perspective_viewport"));
				return BestPerspectiveByArea;
			}
			if (BestAnyByArea)
			{
				SetReason(TEXT("largest_any_viewport"));
				return BestAnyByArea;
			}
			if (FirstValid)
			{
				SetReason(TEXT("first_valid_viewport"));
				return FirstValid;
			}
		}

		SetReason(TEXT("fallback_current_level_editing_viewport"));
		return GCurrentLevelEditingViewportClient;
	}

	static FProperty* FindPropertyByNameLoose(UStruct* StructType, const FString& Segment)
	{
		if (!StructType || Segment.IsEmpty())
		{
			return nullptr;
		}

		const FString Normalized = Segment.TrimStartAndEnd();
		const FString NormalizedWithoutB = Normalized.StartsWith(TEXT("b")) ? Normalized.Mid(1) : Normalized;
		for (TFieldIterator<FProperty> It(StructType, EFieldIterationFlags::IncludeSuper); It; ++It)
		{
			FProperty* Property = *It;
			const FString PropertyName = Property->GetName();
			const FString PropertyNameWithoutB = PropertyName.StartsWith(TEXT("b")) ? PropertyName.Mid(1) : PropertyName;
			if (PropertyName.Equals(Normalized, ESearchCase::IgnoreCase) ||
				PropertyNameWithoutB.Equals(NormalizedWithoutB, ESearchCase::IgnoreCase) ||
				Property->GetAuthoredName().Equals(Normalized, ESearchCase::IgnoreCase))
			{
				return Property;
			}
		}
		return nullptr;
	}

	bool ResolvePropertyPath(UObject* Obj, const FString& PropertyPath, FProperty*& OutProperty, void*& OutValuePtr)
	{
		OutProperty = nullptr;
		OutValuePtr = nullptr;
		if (!Obj || PropertyPath.IsEmpty())
		{
			return false;
		}

		TArray<FString> Segments;
		PropertyPath.ParseIntoArray(Segments, TEXT("."), true);
		if (Segments.Num() == 0)
		{
			return false;
		}

		UStruct* CurrentStruct = Obj->GetClass();
		void* CurrentContainer = Obj;
		for (int32 Index = 0; Index < Segments.Num(); ++Index)
		{
			FProperty* Property = FindPropertyByNameLoose(CurrentStruct, Segments[Index]);
			if (!Property)
			{
				return false;
			}

			void* ValuePtr = Property->ContainerPtrToValuePtr<void>(CurrentContainer);
			if (Index == Segments.Num() - 1)
			{
				OutProperty = Property;
				OutValuePtr = ValuePtr;
				return true;
			}

			FStructProperty* StructProperty = CastField<FStructProperty>(Property);
			if (!StructProperty)
			{
				return false;
			}
			CurrentStruct = StructProperty->Struct;
			CurrentContainer = ValuePtr;
		}

		return false;
	}

	UActorComponent* FindComponentByNameOrPath(AActor* Actor, const FString& ComponentId)
	{
		if (!Actor || ComponentId.IsEmpty())
		{
			return nullptr;
		}

		const FString IdTrim = ComponentId.TrimStartAndEnd();
		TArray<UActorComponent*> Components;
		Actor->GetComponents(Components);
		for (UActorComponent* Comp : Components)
		{
			if (!Comp)
			{
				continue;
			}

			if (Comp->GetName().Equals(IdTrim, ESearchCase::IgnoreCase) ||
				Comp->GetFName().ToString().Equals(IdTrim, ESearchCase::IgnoreCase) ||
				Comp->GetPathName().Equals(IdTrim, ESearchCase::IgnoreCase))
			{
				return Comp;
			}
		}

		return nullptr;
	}

	TSharedPtr<FJsonObject> MakeActorSummaryJson(AActor* Actor)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("name"), Actor ? Actor->GetName() : TEXT(""));
		Item->SetStringField(TEXT("label"), Actor ? Actor->GetActorLabel() : TEXT(""));
		Item->SetStringField(TEXT("class"), (Actor && Actor->GetClass()) ? Actor->GetClass()->GetPathName() : TEXT(""));
		return Item;
	}

	void AugmentActorSummaryWithFolderTags(AActor* Actor, TSharedPtr<FJsonObject>& ActorSummary)
	{
		if (!Actor || !ActorSummary.IsValid())
		{
			return;
		}

		ActorSummary->SetStringField(TEXT("folder_path"), Actor->GetFolderPath().ToString());

		TArray<TSharedPtr<FJsonValue>> TagValues;
		TagValues.Reserve(Actor->Tags.Num());
		for (const FName& Tag : Actor->Tags)
		{
			TagValues.Add(MakeShared<FJsonValueString>(Tag.ToString()));
		}
		ActorSummary->SetArrayField(TEXT("tags"), TagValues);
	}

	TSharedPtr<FJsonObject> MakeHitResultSummaryJson(
		const FHitResult& Hit,
		const bool bIncludeActorFolderTags,
		const bool bIncludePenetrationDepth)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetBoolField(TEXT("blocking_hit"), Hit.bBlockingHit);
		Item->SetBoolField(TEXT("start_penetrating"), Hit.bStartPenetrating);
		Item->SetNumberField(TEXT("time"), Hit.Time);
		Item->SetNumberField(TEXT("distance"), Hit.Distance);
		Item->SetObjectField(TEXT("location"), MakeVectorJson(Hit.ImpactPoint));
		Item->SetObjectField(TEXT("normal"), MakeVectorJson(Hit.ImpactNormal));
		Item->SetStringField(TEXT("bone_name"), Hit.BoneName.ToString());
		Item->SetStringField(TEXT("face_index"), FString::FromInt(Hit.FaceIndex));
		if (bIncludePenetrationDepth && Hit.bStartPenetrating)
		{
			Item->SetNumberField(TEXT("penetration_depth_cm"), Hit.PenetrationDepth);
		}

		if (AActor* HitActor = Hit.GetActor())
		{
			TSharedPtr<FJsonObject> ActorSummary = MakeActorSummaryJson(HitActor);
			if (bIncludeActorFolderTags)
			{
				AugmentActorSummaryWithFolderTags(HitActor, ActorSummary);
			}
			Item->SetObjectField(TEXT("actor"), ActorSummary);
		}
		if (Hit.GetComponent())
		{
			Item->SetStringField(TEXT("component_name"), Hit.GetComponent()->GetName());
			Item->SetStringField(TEXT("component_path"), Hit.GetComponent()->GetPathName());
		}
		return Item;
	}

	void MaybeAttachStartPenetratingAdvice(
		const FString& Context,
		const bool bStartPenetrating,
		const bool bFindInitialOverlaps,
		const bool bIncludeFloorClearanceSuggestion,
		const double FloorClearanceCm,
		TSharedPtr<FJsonObject>& OutData)
	{
		if (!bStartPenetrating || !OutData.IsValid())
		{
			return;
		}

		TSharedPtr<FJsonObject> Advice = MakeShared<FJsonObject>();
		Advice->SetStringField(TEXT("context"), Context);
		Advice->SetStringField(TEXT("reason"), TEXT("start_penetrating"));
		Advice->SetBoolField(TEXT("find_initial_overlaps_current"), bFindInitialOverlaps);

		TArray<TSharedPtr<FJsonValue>> Suggestions;
		{
			TSharedPtr<FJsonObject> S = MakeShared<FJsonObject>();
			S->SetStringField(TEXT("id"), TEXT("raise_start_end_z"));
			S->SetNumberField(TEXT("delta_cm"), 5.0);
			Suggestions.Add(MakeShared<FJsonValueObject>(S));
		}
		{
			TSharedPtr<FJsonObject> S = MakeShared<FJsonObject>();
			S->SetStringField(TEXT("id"), TEXT("set_find_initial_overlaps"));
			S->SetBoolField(TEXT("value"), false);
			Suggestions.Add(MakeShared<FJsonValueObject>(S));
		}
		if (bIncludeFloorClearanceSuggestion)
		{
			TSharedPtr<FJsonObject> S = MakeShared<FJsonObject>();
			S->SetStringField(TEXT("id"), TEXT("increase_floor_clearance_cm"));
			S->SetNumberField(TEXT("current_cm"), FloorClearanceCm);
			S->SetNumberField(TEXT("suggest_min_cm"), 5.0);
			Suggestions.Add(MakeShared<FJsonValueObject>(S));
		}

		Advice->SetArrayField(TEXT("suggestions"), Suggestions);
		OutData->SetObjectField(TEXT("start_penetrating_advice"), Advice);
	}

	void FillActorTransformFields(AActor* Actor, TSharedPtr<FJsonObject>& OutData)
	{
		if (!Actor)
		{
			return;
		}

		OutData->SetStringField(TEXT("actor_name"), Actor->GetName());
		OutData->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
		OutData->SetObjectField(TEXT("location"), MakeVectorJson(Actor->GetActorLocation()));
		OutData->SetObjectField(TEXT("rotation"), MakeRotatorJson(Actor->GetActorRotation()));
		OutData->SetObjectField(TEXT("scale"), MakeVectorJson(Actor->GetActorScale3D()));
		TSharedPtr<FJsonObject> TransformObj = MakeShared<FJsonObject>();
		TransformObj->SetObjectField(TEXT("location"), MakeVectorJson(Actor->GetActorLocation()));
		TransformObj->SetObjectField(TEXT("rotation"), MakeRotatorJson(Actor->GetActorRotation()));
		TransformObj->SetObjectField(TEXT("scale"), MakeVectorJson(Actor->GetActorScale3D()));
		OutData->SetObjectField(TEXT("transform"), TransformObj);
	}

	TSharedPtr<FJsonObject> MakeActorTransformJson(const FTransform& Transform)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetObjectField(TEXT("location"), MakeVectorJson(Transform.GetLocation()));
		Obj->SetObjectField(TEXT("rotation"), MakeRotatorJson(Transform.Rotator()));
		Obj->SetObjectField(TEXT("scale"), MakeVectorJson(Transform.GetScale3D()));
		return Obj;
	}

	static FString SanitizeArtifactSegment(const FString& Value)
	{
		FString Out;
		Out.Reserve(Value.Len());
		for (const TCHAR Ch : Value)
		{
			if (FChar::IsAlnum(Ch) || Ch == TCHAR('-') || Ch == TCHAR('_'))
			{
				Out.AppendChar(Ch);
			}
			else
			{
				Out.AppendChar(TCHAR('_'));
			}
		}
		Out.TrimStartAndEndInline();
		return Out.IsEmpty() ? TEXT("probe") : Out.Left(64);
	}

	FString MakeProbeArtifactPath(const FString& Ext, const FString& RequestId, const FString& Suffix)
	{
		const UUeAgentInterfaceSettings* Settings = UUeAgentInterfaceSettings::Get();
		const FString RootRel = Settings ? Settings->ArtifactsDirRelative : TEXT("Saved/RemoteArtifacts");
		const FString ShotsDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), RootRel / TEXT("Shots"));
		const FString Hostname = SanitizeArtifactSegment(FPlatformProcess::ComputerName());
		const FString Timestamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
		const FString SafeRequestId = SanitizeArtifactSegment(RequestId);
		const FString SafeSuffix = SanitizeArtifactSegment(Suffix);
		const FString Guid = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		return FPaths::Combine(
			ShotsDir,
			FString::Printf(TEXT("probe_%s_%s_%s_%s_%s.%s"), *Hostname, *Timestamp, *SafeRequestId, *SafeSuffix, *Guid, *Ext));
	}

	bool ExportResolvedProperty(UObject* Object, const FString& PropertyPath, FString& OutValueText, FString& OutPropertyClass, FString& OutError)
	{
		FProperty* Property = nullptr;
		void* ValuePtr = nullptr;
		if (!ResolvePropertyPath(Object, PropertyPath, Property, ValuePtr) || !Property || !ValuePtr)
		{
			OutError = TEXT("property_not_found");
			return false;
		}

		OutPropertyClass = Property->GetClass() ? Property->GetClass()->GetName() : TEXT("");
		OutValueText.Reset();
		Property->ExportTextItem_Direct(OutValueText, ValuePtr, nullptr, Object, PPF_None);
		return true;
	}

	void FinishActorTransformEdit(AActor* Actor)
	{
		if (!Actor)
		{
			return;
		}

		Actor->PostEditMove(true);
		Actor->MarkPackageDirty();
	}

	bool ParseBoundsAxis(const FString& AxisText, EBoundsAxis& OutAxis)
	{
		const FString Normalized = AxisText.TrimStartAndEnd().ToLower();
		if (Normalized == TEXT("x"))
		{
			OutAxis = EBoundsAxis::X;
			return true;
		}
		if (Normalized == TEXT("y"))
		{
			OutAxis = EBoundsAxis::Y;
			return true;
		}
		if (Normalized == TEXT("z"))
		{
			OutAxis = EBoundsAxis::Z;
			return true;
		}
		return false;
	}

	bool ParseBoundsAnchor(const FString& AnchorText, EBoundsAnchor& OutAnchor)
	{
		const FString Normalized = AnchorText.TrimStartAndEnd().ToLower();
		if (Normalized == TEXT("min"))
		{
			OutAnchor = EBoundsAnchor::Min;
			return true;
		}
		if (Normalized == TEXT("center") || Normalized == TEXT("centre"))
		{
			OutAnchor = EBoundsAnchor::Center;
			return true;
		}
		if (Normalized == TEXT("max"))
		{
			OutAnchor = EBoundsAnchor::Max;
			return true;
		}
		return false;
	}

	double GetAxisValue(const FVector& Vector, EBoundsAxis Axis)
	{
		switch (Axis)
		{
		case EBoundsAxis::X: return Vector.X;
		case EBoundsAxis::Y: return Vector.Y;
		default: return Vector.Z;
		}
	}

	void SetAxisValue(FVector& Vector, EBoundsAxis Axis, double Value)
	{
		switch (Axis)
		{
		case EBoundsAxis::X: Vector.X = (float)Value; break;
		case EBoundsAxis::Y: Vector.Y = (float)Value; break;
		default: Vector.Z = (float)Value; break;
		}
	}

	bool GetActorBoundsBox(AActor* Actor, FBox& OutBox, FString& OutError)
	{
		if (!Actor)
		{
			OutError = TEXT("actor_not_found");
			return false;
		}

		OutBox = Actor->GetComponentsBoundingBox(true);
		if (!OutBox.IsValid)
		{
			OutError = TEXT("actor_has_invalid_bounds");
			return false;
		}
		return true;
	}

	double GetBoundsAnchorValue(const FBox& Box, EBoundsAxis Axis, EBoundsAnchor Anchor)
	{
		const FVector Center = Box.GetCenter();
		const FVector Min = Box.Min;
		const FVector Max = Box.Max;
		switch (Anchor)
		{
		case EBoundsAnchor::Min:
			return GetAxisValue(Min, Axis);
		case EBoundsAnchor::Center:
			return GetAxisValue(Center, Axis);
		default:
			return GetAxisValue(Max, Axis);
		}
	}

	TSharedPtr<FJsonObject> MakeBoundsJson(const FBox& Box)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetObjectField(TEXT("min"), MakeVectorJson(Box.Min));
		Obj->SetObjectField(TEXT("max"), MakeVectorJson(Box.Max));
		Obj->SetObjectField(TEXT("center"), MakeVectorJson(Box.GetCenter()));
		Obj->SetObjectField(TEXT("extent"), MakeVectorJson(Box.GetExtent()));
		return Obj;
	}

	FString NormalizeFolderPathForMatch(const FString& InFolderPath)
	{
		FString Normalized = InFolderPath.TrimStartAndEnd();
		Normalized.ReplaceInline(TEXT("\\"), TEXT("/"));
		while (Normalized.RemoveFromStart(TEXT("/")))
		{
		}
		while (Normalized.RemoveFromEnd(TEXT("/")))
		{
		}
		return Normalized;
	}

	bool DoesActorMatchFolderPath(AActor* Actor, const FString& NormalizedFolderPath, bool bIncludeChildFolders)
	{
		if (!Actor || NormalizedFolderPath.IsEmpty())
		{
			return false;
		}

		const FString ActorFolderPath = NormalizeFolderPathForMatch(Actor->GetFolderPath().ToString());
		if (ActorFolderPath.IsEmpty())
		{
			return false;
		}

		if (ActorFolderPath.Equals(NormalizedFolderPath, ESearchCase::IgnoreCase))
		{
			return true;
		}

		if (!bIncludeChildFolders)
		{
			return false;
		}

		return ActorFolderPath.StartsWith(NormalizedFolderPath + TEXT("/"), ESearchCase::IgnoreCase);
	}

	void AppendIgnoredActorsFromFilters(UWorld* World, const TSharedPtr<FJsonObject>& Params, TArray<AActor*>& InOutIgnoredActors)
	{
		if (!World || !Params.IsValid())
		{
			return;
		}

		FString IgnoreFolderPathPrefix;
		TryGetString(Params, TEXT("ignore_folder_path_prefix"), IgnoreFolderPathPrefix);
		IgnoreFolderPathPrefix = IgnoreFolderPathPrefix.TrimStartAndEnd();
		const FString NormalizedIgnoreFolderPrefix = NormalizeFolderPathForMatch(IgnoreFolderPathPrefix);
		const bool bHasIgnoreFolderFilter = !NormalizedIgnoreFolderPrefix.IsEmpty();

		auto ParseStringArray = [&Params](const FString& FieldName, TArray<FString>& OutStrings)
		{
			OutStrings.Reset();
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Params.IsValid() || !Params->TryGetArrayField(FieldName, Values) || !Values)
			{
				return;
			}

			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				if (!Value.IsValid())
				{
					continue;
				}
				const FString Str = Value->AsString().TrimStartAndEnd();
				if (!Str.IsEmpty())
				{
					OutStrings.Add(Str);
				}
			}
		};

		TArray<FString> IgnoreTags;
		TArray<FString> IgnoreClassSubstrings;
		ParseStringArray(TEXT("ignore_tags"), IgnoreTags);
		ParseStringArray(TEXT("ignore_class_substrings"), IgnoreClassSubstrings);

		if (!bHasIgnoreFolderFilter && IgnoreTags.Num() <= 0 && IgnoreClassSubstrings.Num() <= 0)
		{
			return;
		}

		TSet<AActor*> IgnoreSet;
		IgnoreSet.Reserve(InOutIgnoredActors.Num() + 64);
		for (AActor* Actor : InOutIgnoredActors)
		{
			if (Actor)
			{
				IgnoreSet.Add(Actor);
			}
		}

		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor || Actor->IsActorBeingDestroyed())
			{
				continue;
			}

			bool bIgnored = false;
			if (bHasIgnoreFolderFilter && DoesActorMatchFolderPath(Actor, NormalizedIgnoreFolderPrefix, /*bIncludeChildFolders*/ true))
			{
				bIgnored = true;
			}

			if (!bIgnored && IgnoreTags.Num() > 0)
			{
				for (const FString& TagText : IgnoreTags)
				{
					if (!TagText.IsEmpty() && Actor->ActorHasTag(*TagText))
					{
						bIgnored = true;
						break;
					}
				}
			}

			if (!bIgnored && IgnoreClassSubstrings.Num() > 0)
			{
				const FString ActorClassPath = (Actor->GetClass() ? Actor->GetClass()->GetPathName() : FString());
				for (const FString& Sub : IgnoreClassSubstrings)
				{
					if (!Sub.IsEmpty() && ActorClassPath.Contains(Sub, ESearchCase::IgnoreCase))
					{
						bIgnored = true;
						break;
					}
				}
			}

			if (bIgnored)
			{
				IgnoreSet.Add(Actor);
			}
		}

		InOutIgnoredActors.Reset(IgnoreSet.Num());
		for (AActor* Actor : IgnoreSet)
		{
			if (Actor)
			{
				InOutIgnoredActors.Add(Actor);
			}
		}
	}

	bool GetFolderActorBounds(
		UWorld* World,
		const FString& FolderPath,
		bool bIncludeChildFolders,
		FBox& OutCombinedBounds,
		TArray<AActor*>& OutActors,
		FString& OutError)
	{
		if (!World)
		{
			OutError = TEXT("missing_world");
			return false;
		}

		const FString NormalizedFolderPath = NormalizeFolderPathForMatch(FolderPath);
		if (NormalizedFolderPath.IsEmpty())
		{
			OutError = TEXT("missing_folder_path");
			return false;
		}

		OutCombinedBounds = FBox(EForceInit::ForceInit);
		OutActors.Reset();
		bool bFoundFolderActor = false;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor || Actor->IsActorBeingDestroyed())
			{
				continue;
			}
			if (!DoesActorMatchFolderPath(Actor, NormalizedFolderPath, bIncludeChildFolders))
			{
				continue;
			}

			bFoundFolderActor = true;
			FBox ActorBounds(EForceInit::ForceInit);
			FString BoundsError;
			if (!GetActorBoundsBox(Actor, ActorBounds, BoundsError))
			{
				continue;
			}

			OutCombinedBounds += ActorBounds;
			OutActors.Add(Actor);
		}

		if (OutActors.Num() <= 0)
		{
			OutError = bFoundFolderActor ? TEXT("folder_actor_bounds_invalid") : TEXT("folder_has_no_actors");
			return false;
		}

		if (!OutCombinedBounds.IsValid)
		{
			OutError = TEXT("folder_bounds_invalid");
			return false;
		}

		return true;
	}

	bool GetActorsInFolder(
		UWorld* World,
		const FString& FolderPath,
		bool bIncludeChildFolders,
		TArray<AActor*>& OutActors,
		FString& OutError)
	{
		if (!World)
		{
			OutError = TEXT("missing_world");
			return false;
		}

		const FString NormalizedFolderPath = NormalizeFolderPathForMatch(FolderPath);
		if (NormalizedFolderPath.IsEmpty())
		{
			OutError = TEXT("missing_folder_path");
			return false;
		}

		OutActors.Reset();
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor || Actor->IsActorBeingDestroyed())
			{
				continue;
			}
			if (!DoesActorMatchFolderPath(Actor, NormalizedFolderPath, bIncludeChildFolders))
			{
				continue;
			}

			OutActors.Add(Actor);
		}

		return true;
	}

	static void GetBoxCorners(const FBox& Box, TArray<FVector>& OutCorners)
	{
		OutCorners.Reset(8);
		const FVector& Min = Box.Min;
		const FVector& Max = Box.Max;
		OutCorners.Add(FVector(Min.X, Min.Y, Min.Z));
		OutCorners.Add(FVector(Min.X, Min.Y, Max.Z));
		OutCorners.Add(FVector(Min.X, Max.Y, Min.Z));
		OutCorners.Add(FVector(Min.X, Max.Y, Max.Z));
		OutCorners.Add(FVector(Max.X, Min.Y, Min.Z));
		OutCorners.Add(FVector(Max.X, Min.Y, Max.Z));
		OutCorners.Add(FVector(Max.X, Max.Y, Min.Z));
		OutCorners.Add(FVector(Max.X, Max.Y, Max.Z));
	}

	bool ComputeViewportLocationToFrameBounds(
		FLevelEditorViewportClient* ViewportClient,
		const FBox& Bounds,
		const FRotator& ViewRotation,
		double Padding,
		FVector& OutLocation,
		FString& OutError)
	{
		if (!ViewportClient || !ViewportClient->Viewport)
		{
			OutError = TEXT("no_level_editor_viewport");
			return false;
		}

		const FIntPoint Size = ViewportClient->Viewport->GetSizeXY();
		if (Size.X <= 0 || Size.Y <= 0)
		{
			OutError = TEXT("invalid_viewport_size");
			return false;
		}

		if (ViewportClient->ViewportType != LVT_Perspective)
		{
			OutError = TEXT("viewport_not_perspective");
			return false;
		}

		const double AspectRatio = static_cast<double>(Size.X) / static_cast<double>(Size.Y);
		if (AspectRatio <= KINDA_SMALL_NUMBER)
		{
			OutError = TEXT("invalid_viewport_aspect_ratio");
			return false;
		}

		Padding = FMath::Clamp(Padding, 1.0, 4.0);
		const double HorizontalHalfFovRadians = FMath::DegreesToRadians(FMath::Clamp(static_cast<double>(ViewportClient->ViewFOV), 5.0, 170.0) * 0.5);
		const double TanHalfHorizontalFov = FMath::Tan(HorizontalHalfFovRadians);
		const double TanHalfVerticalFov = TanHalfHorizontalFov / AspectRatio;
		if (TanHalfHorizontalFov <= KINDA_SMALL_NUMBER || TanHalfVerticalFov <= KINDA_SMALL_NUMBER)
		{
			OutError = TEXT("invalid_viewport_fov");
			return false;
		}

		const FVector BoundsCenter = Bounds.GetCenter();
		const FRotationMatrix RotationMatrix(ViewRotation);
		const FVector Forward = RotationMatrix.GetUnitAxis(EAxis::X);
		const FVector Right = RotationMatrix.GetUnitAxis(EAxis::Y);
		const FVector Up = RotationMatrix.GetUnitAxis(EAxis::Z);

		TArray<FVector> Corners;
		GetBoxCorners(Bounds, Corners);

		double RequiredDistance = 1.0;
		for (const FVector& Corner : Corners)
		{
			const FVector Relative = Corner - BoundsCenter;
			const double ForwardOffset = FVector::DotProduct(Relative, Forward);
			const double RightOffset = FMath::Abs(FVector::DotProduct(Relative, Right));
			const double UpOffset = FMath::Abs(FVector::DotProduct(Relative, Up));

			RequiredDistance = FMath::Max(RequiredDistance, (RightOffset / TanHalfHorizontalFov) - ForwardOffset);
			RequiredDistance = FMath::Max(RequiredDistance, (UpOffset / TanHalfVerticalFov) - ForwardOffset);
		}

		OutLocation = BoundsCenter - (Forward * FMath::Max(RequiredDistance * Padding, 1.0));
		return true;
	}

	bool ResolveViewportScreenPoint(FLevelEditorViewportClient* ViewportClient, const TSharedPtr<FJsonObject>& Params, FVector2D& OutScreenPoint, FString& OutError)
	{
		if (!ViewportClient || !ViewportClient->Viewport)
		{
			OutError = TEXT("no_level_editor_viewport");
			return false;
		}

		double ScreenX = 0.0;
		double ScreenY = 0.0;
		if (!TryGetNumber(Params, TEXT("screen_x"), ScreenX) ||
			!TryGetNumber(Params, TEXT("screen_y"), ScreenY))
		{
			OutError = TEXT("missing_screen_point");
			return false;
		}

		const FIntPoint Size = ViewportClient->Viewport->GetSizeXY();
		if (Size.X <= 0 || Size.Y <= 0)
		{
			OutError = TEXT("invalid_viewport_size");
			return false;
		}

		OutScreenPoint.X = (float)FMath::Clamp(ScreenX, 0.0, (double)Size.X);
		OutScreenPoint.Y = (float)FMath::Clamp(ScreenY, 0.0, (double)Size.Y);
		return true;
	}

	bool ResolveTraceChannel(const TSharedPtr<FJsonObject>& Params, ECollisionChannel& OutChannel)
	{
		OutChannel = ECC_Visibility;

		FString ChannelName;
		if (!TryGetString(Params, TEXT("trace_channel"), ChannelName) || ChannelName.IsEmpty())
		{
			return true;
		}

		const FString Normalized = ChannelName.TrimStartAndEnd().ToLower();
		if (Normalized == TEXT("visibility") || Normalized == TEXT("ecc_visibility"))
		{
			OutChannel = ECC_Visibility;
			return true;
		}
		if (Normalized == TEXT("camera") || Normalized == TEXT("ecc_camera"))
		{
			OutChannel = ECC_Camera;
			return true;
		}
		if (Normalized == TEXT("worldstatic") || Normalized == TEXT("world_static") || Normalized == TEXT("ecc_worldstatic"))
		{
			OutChannel = ECC_WorldStatic;
			return true;
		}
		if (Normalized == TEXT("worlddynamic") || Normalized == TEXT("world_dynamic") || Normalized == TEXT("ecc_worlddynamic"))
		{
			OutChannel = ECC_WorldDynamic;
			return true;
		}
		if (Normalized == TEXT("pawn") || Normalized == TEXT("ecc_pawn"))
		{
			OutChannel = ECC_Pawn;
			return true;
		}
		if (Normalized == TEXT("physicsbody") || Normalized == TEXT("physics_body") || Normalized == TEXT("ecc_physicsbody"))
		{
			OutChannel = ECC_PhysicsBody;
			return true;
		}

		return false;
	}

	void FillHitResultJson(const FHitResult& Hit, TSharedPtr<FJsonObject>& OutData)
	{
		OutData->SetBoolField(TEXT("hit"), Hit.bBlockingHit);
		if (!Hit.bBlockingHit)
		{
			return;
		}

		OutData->SetObjectField(TEXT("location"), MakeVectorJson(Hit.ImpactPoint));
		OutData->SetObjectField(TEXT("normal"), MakeVectorJson(Hit.ImpactNormal));
		OutData->SetNumberField(TEXT("distance"), Hit.Distance);
		OutData->SetStringField(TEXT("bone_name"), Hit.BoneName.ToString());
		OutData->SetStringField(TEXT("face_index"), FString::FromInt(Hit.FaceIndex));
		if (Hit.GetActor())
		{
			OutData->SetStringField(TEXT("actor_id"), Hit.GetActor()->GetActorLabel());
			OutData->SetStringField(TEXT("actor_name"), Hit.GetActor()->GetName());
		}
		if (Hit.GetComponent())
		{
			OutData->SetStringField(TEXT("component_name"), Hit.GetComponent()->GetName());
			OutData->SetStringField(TEXT("component_path"), Hit.GetComponent()->GetPathName());
		}
	}

	bool TraceActorFromScreenPoint(
		UWorld* World,
		FLevelEditorViewportClient* ViewportClient,
		const TSharedPtr<FJsonObject>& Params,
		FVector2D& OutScreenPoint,
		FVector& OutWorldOrigin,
		FVector& OutWorldDirection,
		double& OutTraceDistance,
		ECollisionChannel& OutTraceChannel,
		bool& OutTraceComplex,
		FHitResult& OutHit,
		FString& OutError)
	{
		if (!World)
		{
			OutError = TEXT("missing_world");
			return false;
		}
		if (!ViewportClient || !ViewportClient->Viewport)
		{
			OutError = TEXT("no_level_editor_viewport");
			return false;
		}

		if (!ResolveViewportScreenPoint(ViewportClient, Params, OutScreenPoint, OutError))
		{
			return false;
		}

		FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(
			ViewportClient->Viewport,
			World->Scene,
			ViewportClient->EngineShowFlags).SetRealtimeUpdate(ViewportClient->IsRealtime()));
		FSceneView* SceneView = ViewportClient->CalcSceneView(&ViewFamily);
		if (!SceneView)
		{
			OutError = TEXT("calc_scene_view_failed");
			return false;
		}

		OutWorldOrigin = FVector::ZeroVector;
		OutWorldDirection = FVector::ForwardVector;
		SceneView->DeprojectFVector2D(OutScreenPoint, OutWorldOrigin, OutWorldDirection);

		OutTraceDistance = HALF_WORLD_MAX;
		TryGetNumber(Params, TEXT("trace_distance"), OutTraceDistance);
		OutTraceDistance = FMath::Clamp(OutTraceDistance, 1.0, static_cast<double>(HALF_WORLD_MAX));

		OutTraceChannel = ECC_Visibility;
		if (!ResolveTraceChannel(Params, OutTraceChannel))
		{
			OutError = TEXT("unsupported_trace_channel");
			return false;
		}

		OutTraceComplex = true;
		Params.IsValid() && Params->TryGetBoolField(TEXT("trace_complex"), OutTraceComplex);

		bool bAllowNoHit = false;
		Params.IsValid() && Params->TryGetBoolField(TEXT("allow_no_hit"), bAllowNoHit);

		FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(UeAgentPickActorAtScreen), OutTraceComplex);
		if (Params.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* IgnoreIds = nullptr;
			if (Params->TryGetArrayField(TEXT("ignore_actor_ids"), IgnoreIds) && IgnoreIds)
			{
				auto FindIgnoreActor = [](UWorld* InWorld, const FString& InId) -> AActor*
				{
					if (!InWorld || InId.TrimStartAndEnd().IsEmpty())
					{
						return nullptr;
					}

					const FString IdTrim = InId.TrimStartAndEnd();
					for (TActorIterator<AActor> It(InWorld); It; ++It)
					{
						AActor* Actor = *It;
						if (!Actor || Actor->IsActorBeingDestroyed())
						{
							continue;
						}
						if (Actor->GetName().Equals(IdTrim, ESearchCase::IgnoreCase) ||
							Actor->GetActorLabel().Equals(IdTrim, ESearchCase::IgnoreCase))
						{
							return Actor;
						}
					}
					return nullptr;
				};

				for (const TSharedPtr<FJsonValue>& IgnoreValue : *IgnoreIds)
				{
					const FString IgnoreId = IgnoreValue.IsValid() ? IgnoreValue->AsString() : FString();
					if (AActor* IgnoreActor = FindIgnoreActor(World, IgnoreId))
					{
						QueryParams.AddIgnoredActor(IgnoreActor);
					}
				}
			}
		}
		const FVector TraceEnd = OutWorldOrigin + (OutWorldDirection * OutTraceDistance);
		const bool bHit = World->LineTraceSingleByChannel(OutHit, OutWorldOrigin, TraceEnd, OutTraceChannel, QueryParams);
		if (!bHit)
		{
			if (bAllowNoHit)
			{
				OutHit = FHitResult();
				return true;
			}

			OutError = TEXT("actor_not_hit");
			return false;
		}

		if (!OutHit.GetActor())
		{
			if (bAllowNoHit)
			{
				return true;
			}

			OutError = TEXT("hit_actor_missing");
			return false;
		}

		return true;
	}

	bool ComputeActorLocalObbBox(AActor* Actor, FBox& OutLocalBox, FString& OutError)
	{
		if (!Actor)
		{
			OutError = TEXT("actor_not_found");
			return false;
		}

		const FTransform ActorTransform = Actor->GetActorTransform();
		OutLocalBox = FBox(EForceInit::ForceInit);
		bool bAddedAnyCorner = false;

		TInlineComponentArray<UPrimitiveComponent*> PrimitiveComponents(Actor);
		for (UPrimitiveComponent* PrimitiveComponent : PrimitiveComponents)
		{
			if (!PrimitiveComponent || !PrimitiveComponent->IsRegistered())
			{
				continue;
			}

			const FBoxSphereBounds LocalBounds = PrimitiveComponent->CalcBounds(FTransform::Identity);
			const FBox PrimitiveLocalBox(LocalBounds.Origin - LocalBounds.BoxExtent, LocalBounds.Origin + LocalBounds.BoxExtent);

			TArray<FVector> PrimitiveCorners;
			GetBoxCorners(PrimitiveLocalBox, PrimitiveCorners);
			for (const FVector& PrimitiveLocalCorner : PrimitiveCorners)
			{
				const FVector WorldCorner = PrimitiveComponent->GetComponentTransform().TransformPosition(PrimitiveLocalCorner);
				const FVector ActorLocalCorner = ActorTransform.InverseTransformPosition(WorldCorner);
				OutLocalBox += ActorLocalCorner;
				bAddedAnyCorner = true;
			}
		}

		if (!bAddedAnyCorner)
		{
			FBox WorldBounds(EForceInit::ForceInit);
			if (!GetActorBoundsBox(Actor, WorldBounds, OutError))
			{
				return false;
			}

			TArray<FVector> WorldCorners;
			GetBoxCorners(WorldBounds, WorldCorners);
			for (const FVector& WorldCorner : WorldCorners)
			{
				OutLocalBox += ActorTransform.InverseTransformPosition(WorldCorner);
			}
			bAddedAnyCorner = true;
		}

		if (!bAddedAnyCorner || !OutLocalBox.IsValid)
		{
			OutError = TEXT("actor_obb_invalid");
			return false;
		}

		return true;
	}

	TSharedPtr<FJsonObject> MakeActorObbJson(AActor* Actor, const FBox& LocalBox)
	{
		const FTransform ActorTransform = Actor->GetActorTransform();
		const FVector LocalCenter = LocalBox.GetCenter();
		const FVector LocalExtent = LocalBox.GetExtent();

		const FVector AxisXScaled = ActorTransform.TransformVector(FVector(1.0f, 0.0f, 0.0f));
		const FVector AxisYScaled = ActorTransform.TransformVector(FVector(0.0f, 1.0f, 0.0f));
		const FVector AxisZScaled = ActorTransform.TransformVector(FVector(0.0f, 0.0f, 1.0f));

		const FVector AxisX = AxisXScaled.GetSafeNormal();
		const FVector AxisY = AxisYScaled.GetSafeNormal();
		const FVector AxisZ = AxisZScaled.GetSafeNormal();

		const double HalfLengthX = LocalExtent.X * AxisXScaled.Size();
		const double HalfLengthY = LocalExtent.Y * AxisYScaled.Size();
		const double HalfLengthZ = LocalExtent.Z * AxisZScaled.Size();

		const FVector WorldCenter = ActorTransform.TransformPosition(LocalCenter);

		TArray<TSharedPtr<FJsonValue>> CornerValues;
		CornerValues.Reserve(8);
		for (int32 SignX = -1; SignX <= 1; SignX += 2)
		{
			for (int32 SignY = -1; SignY <= 1; SignY += 2)
			{
				for (int32 SignZ = -1; SignZ <= 1; SignZ += 2)
				{
					const FVector Corner =
						WorldCenter +
						(AxisX * (HalfLengthX * static_cast<double>(SignX))) +
						(AxisY * (HalfLengthY * static_cast<double>(SignY))) +
						(AxisZ * (HalfLengthZ * static_cast<double>(SignZ)));
					CornerValues.Add(MakeShared<FJsonValueObject>(MakeVectorJson(Corner)));
				}
			}
		}

		TSharedPtr<FJsonObject> HalfLengthsJson = MakeShared<FJsonObject>();
		HalfLengthsJson->SetNumberField(TEXT("x"), HalfLengthX);
		HalfLengthsJson->SetNumberField(TEXT("y"), HalfLengthY);
		HalfLengthsJson->SetNumberField(TEXT("z"), HalfLengthZ);

		TSharedPtr<FJsonObject> ObbJson = MakeShared<FJsonObject>();
		ObbJson->SetObjectField(TEXT("center"), MakeVectorJson(WorldCenter));
		ObbJson->SetObjectField(TEXT("axis_x"), MakeVectorJson(AxisX));
		ObbJson->SetObjectField(TEXT("axis_y"), MakeVectorJson(AxisY));
		ObbJson->SetObjectField(TEXT("axis_z"), MakeVectorJson(AxisZ));
		ObbJson->SetObjectField(TEXT("half_lengths"), HalfLengthsJson);
		ObbJson->SetObjectField(TEXT("local_center"), MakeVectorJson(LocalCenter));
		ObbJson->SetObjectField(TEXT("local_extent"), MakeVectorJson(LocalExtent));
		ObbJson->SetArrayField(TEXT("corners"), CornerValues);
		return ObbJson;
	}

	TSharedPtr<FJsonObject> MakeActorInfoJson(AActor* Actor)
	{
		TSharedPtr<FJsonObject> Info = MakeActorSummaryJson(Actor);
		if (!Actor)
		{
			return Info;
		}

		Info->SetStringField(TEXT("folder_path"), Actor->GetFolderPath().ToString());
		FillActorTransformFields(Actor, Info);

		TArray<TSharedPtr<FJsonValue>> TagValues;
		TagValues.Reserve(Actor->Tags.Num());
		for (const FName& Tag : Actor->Tags)
		{
			TagValues.Add(MakeShared<FJsonValueString>(Tag.ToString()));
		}
		Info->SetArrayField(TEXT("tags"), TagValues);

		FString BoundsError;
		FBox Bounds(EForceInit::ForceInit);
		if (GetActorBoundsBox(Actor, Bounds, BoundsError))
		{
			Info->SetObjectField(TEXT("bounds"), MakeBoundsJson(Bounds));
		}

		FString ObbError;
		FBox LocalObbBox(EForceInit::ForceInit);
		if (ComputeActorLocalObbBox(Actor, LocalObbBox, ObbError))
		{
			Info->SetObjectField(TEXT("obb"), MakeActorObbJson(Actor, LocalObbBox));
		}

		return Info;
	}

	bool GetStaticMeshComponentForVertexQuery(AActor* Actor, const FString& ComponentId, UStaticMeshComponent*& OutComponent, FString& OutError)
	{
		OutComponent = nullptr;
		if (!Actor)
		{
			OutError = TEXT("actor_not_found");
			return false;
		}

		if (!ComponentId.IsEmpty())
		{
			OutComponent = Cast<UStaticMeshComponent>(FindComponentByNameOrPath(Actor, ComponentId));
		}
		if (!OutComponent)
		{
			OutComponent = Actor->FindComponentByClass<UStaticMeshComponent>();
		}
		if (!OutComponent)
		{
			OutError = TEXT("static_mesh_component_not_found");
			return false;
		}
		if (!OutComponent->GetStaticMesh())
		{
			OutError = TEXT("static_mesh_not_found");
			return false;
		}
		return true;
	}

	bool GetStaticMeshVertexLocalPositions(UStaticMesh* StaticMesh, TArray<FVector>& OutPositions, FString& OutError)
	{
		OutPositions.Reset();
		if (!StaticMesh)
		{
			OutError = TEXT("static_mesh_not_found");
			return false;
		}

		FMeshDescription* MeshDescription = StaticMesh->GetMeshDescription(0);
		if (!MeshDescription)
		{
			OutError = TEXT("mesh_description_not_found");
			return false;
		}

		FStaticMeshAttributes Attributes(*MeshDescription);
		TVertexAttributesConstRef<FVector3f> VertexPositions = Attributes.GetVertexPositions();
		for (const FVertexID VertexId : MeshDescription->Vertices().GetElementIDs())
		{
			OutPositions.Add((FVector)VertexPositions[VertexId]);
		}

		if (OutPositions.Num() <= 0)
		{
			OutError = TEXT("vertex_positions_empty");
			return false;
		}
		return true;
	}

	bool ResolveVertexWorldPosition(UStaticMeshComponent* Component, const TArray<FVector>& LocalPositions, int32 VertexIndex, FVector& OutWorldPosition, FString& OutError)
	{
		if (!Component)
		{
			OutError = TEXT("static_mesh_component_not_found");
			return false;
		}
		if (!LocalPositions.IsValidIndex(VertexIndex))
		{
			OutError = TEXT("vertex_index_out_of_range");
			return false;
		}

		OutWorldPosition = Component->GetComponentTransform().TransformPosition(LocalPositions[VertexIndex]);
		return true;
	}
}


