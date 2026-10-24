// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/EditorViewport/UeAgentViewportViewMode.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Dom/JsonValue.h"

#include "BufferVisualizationData.h"
#include "EditorViewportClient.h"
#include "Engine/Engine.h"
#include "GameFramework/ActorPrimitiveColorHandler.h"
#include "GPUSkinCacheVisualizationData.h"
#include "GroomVisualizationData.h"
#include "LumenVisualizationData.h"
#include "NaniteVisualizationData.h"
#include "RayTracingVisualizationData.h"
#include "SubstrateVisualizationData.h"
#include "VirtualShadowMapVisualizationData.h"
#include "Dom/JsonObject.h"
#include "Materials/MaterialInterface.h"

namespace UeAgentViewportViewMode
{
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

	FString ViewModeIndexToDisplayName(EViewModeIndex ViewMode)
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

	static FResolvedViewportViewMode MakeResolvedSimpleViewMode(const FSimpleViewportViewModeEntry& Entry)
	{
		FResolvedViewportViewMode Resolved;
		Resolved.Key = Entry.Key;
		Resolved.Category = Entry.Category;
		Resolved.Label = Entry.Label;
		Resolved.ViewMode = Entry.ViewMode;
		Resolved.bFinalLitComposition = IsFinalLitCompositionViewMode(Entry.ViewMode);
		return Resolved;
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
		// Native viewports can retain an unset selector in any visualization family.
		// Replay that state explicitly instead of replacing it with the family's default.
		if (NormalizeViewportModeToken(InMode) == TEXT("none"))
		{
			OutModeName = NAME_None;
			OutLabel = Family == EViewportVisualizationFamily::Buffer ? TEXT("Overview") : TEXT("None");
			return true;
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

	static FString MakeVisualizationModeKey(EViewportVisualizationFamily Family, FName ModeName)
	{
		const FString ModeKey = ModeName.IsNone()
			? (Family == EViewportVisualizationFamily::Buffer ? TEXT("overview") : TEXT("none"))
			: ModeName.ToString();
		return MakeModeKeySegment(VisualizationFamilyToString(Family) + TEXT("_") + ModeKey);
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
		Resolved.Key = MakeVisualizationModeKey(Family, ModeName);
		Resolved.bFinalLitComposition = false;
		return Resolved;
	}

	static bool TryResolveViewportViewModeString(const FString& InMode, FResolvedViewportViewMode& OutResolved, UE::UAI::Contract::FDiagnostic& OutError)
	{
		const FString ModeTrim = InMode.TrimStartAndEnd();
		const FString Wanted = NormalizeViewportModeToken(ModeTrim);
		if (Wanted.IsEmpty())
		{
			OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("missing_view_mode"), TEXT("view_mode"),
				MakeShared<FJsonValueString>(TEXT("nonempty normalized view mode token")), MakeShared<FJsonValueString>(InMode),
				TEXT("input_decode"), TEXT("The view mode contains no usable characters after normalization."));
			return false;
		}

		for (const FSimpleViewportViewModeEntry& Entry : GetSimpleViewportViewModeEntries())
		{
			if (NormalizeViewportModeToken(Entry.Key) == Wanted
				|| NormalizeViewportModeToken(Entry.Label) == Wanted
				|| NormalizeViewportModeToken(ViewModeIndexToStableName(Entry.ViewMode)) == Wanted)
			{
				OutResolved = MakeResolvedSimpleViewMode(Entry);
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
				OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("unsupported_visualization_mode"), TEXT("view_mode"), MakeShared<FJsonValueString>(TEXT("registered mode in ") + FamilyPart), MakeShared<FJsonValueString>(ModeTrim));
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
			OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("ambiguous_visualization_mode"), TEXT("view_mode"), MakeShared<FJsonValueString>(TEXT("family:mode")), MakeShared<FJsonValueString>(ModeTrim), TEXT("input_decode"), TEXT("The mode name matches multiple visualization families; specify the family."));
			return false;
		}

		OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("unsupported_view_mode"), TEXT("view_mode"), MakeShared<FJsonValueString>(TEXT("registered viewport mode or family:mode")), MakeShared<FJsonValueString>(ModeTrim));
		return false;
	}

	bool TryResolveRequestedViewportViewMode(
		const TSharedPtr<FJsonObject>& Params,
		bool& bOutHasViewMode,
		FResolvedViewportViewMode& OutResolved,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		bOutHasViewMode = false;
		if (!Params.IsValid())
		{
			return true;
		}

		const auto ReadNamedString = [&Params](const TCHAR* Field, FString& Value, const TCHAR*& OutField)
		{
			if (!Params->TryGetStringField(Field, Value)) return false;
			OutField = Field;
			return true;
		};
		FString FamilyText;
		const TCHAR* FamilyField = TEXT("visualization_family");
		if (ReadNamedString(TEXT("visualization_family"), FamilyText, FamilyField)
			|| ReadNamedString(TEXT("visualizer"), FamilyText, FamilyField)
			|| ReadNamedString(TEXT("view_mode_family"), FamilyText, FamilyField))
		{
			EViewportVisualizationFamily Family = EViewportVisualizationFamily::None;
			if (!TryParseVisualizationFamily(FamilyText, Family))
			{
				OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("unsupported_visualization_family"), FamilyField, MakeShared<FJsonValueString>(TEXT("registered visualization family")), MakeShared<FJsonValueString>(FamilyText));
				return false;
			}

			FString ModeText;
			const TCHAR* ModeField = TEXT("visualization_mode");
			ReadNamedString(TEXT("visualization_mode"), ModeText, ModeField)
				|| ReadNamedString(TEXT("visualizer_mode"), ModeText, ModeField)
				|| ReadNamedString(TEXT("debug_mode"), ModeText, ModeField);

			FName ModeName = NAME_None;
			FString Label;
			if (!TryResolveVisualizationModeInFamily(Family, ModeText, ModeName, Label))
			{
				OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("unsupported_visualization_mode"), ModeField, MakeShared<FJsonValueString>(TEXT("registered mode in ") + FamilyText), MakeShared<FJsonValueString>(ModeText));
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
					OutError.PropertyPath = Field;
					OutError.SemanticPointer = Field;
					OutError.Source.JsonPath = Field;
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

	FResolvedViewportViewMode DescribeViewportViewModeState(const FViewportViewModeState& State)
	{
		EViewportVisualizationFamily ActiveVisualizationFamily = EViewportVisualizationFamily::None;
		FName ActiveVisualizationMode = NAME_None;
		switch (State.ViewMode)
		{
		case VMI_VisualizeBuffer:
			ActiveVisualizationFamily = EViewportVisualizationFamily::Buffer;
			ActiveVisualizationMode = State.BufferMode;
			break;
		case VMI_VisualizeNanite:
			ActiveVisualizationFamily = EViewportVisualizationFamily::Nanite;
			ActiveVisualizationMode = State.NaniteMode;
			break;
		case VMI_VisualizeLumen:
			ActiveVisualizationFamily = EViewportVisualizationFamily::Lumen;
			ActiveVisualizationMode = State.LumenMode;
			break;
		case VMI_VisualizeSubstrate:
			ActiveVisualizationFamily = EViewportVisualizationFamily::Substrate;
			ActiveVisualizationMode = State.SubstrateMode;
			break;
		case VMI_VisualizeGroom:
			ActiveVisualizationFamily = EViewportVisualizationFamily::Groom;
			ActiveVisualizationMode = State.GroomMode;
			break;
		case VMI_VisualizeVirtualShadowMap:
			ActiveVisualizationFamily = EViewportVisualizationFamily::VirtualShadowMap;
			ActiveVisualizationMode = State.VirtualShadowMapMode;
			break;
		case VMI_VisualizeVirtualTexture:
			ActiveVisualizationFamily = EViewportVisualizationFamily::VirtualTexture;
			ActiveVisualizationMode = State.VirtualTextureMode;
			break;
		case VMI_VisualizeActorColoration:
			ActiveVisualizationFamily = EViewportVisualizationFamily::ActorColoration;
			ActiveVisualizationMode = State.ActorColorationMode;
			break;
		case VMI_VisualizeGPUSkinCache:
			ActiveVisualizationFamily = EViewportVisualizationFamily::GPUSkinCache;
			ActiveVisualizationMode = State.GPUSkinCacheMode;
			break;
		case VMI_RayTracingDebug:
			ActiveVisualizationFamily = EViewportVisualizationFamily::RayTracingDebug;
			ActiveVisualizationMode = State.RayTracingDebugMode;
			break;
		default:
			break;
		}
		if (ActiveVisualizationFamily != EViewportVisualizationFamily::None)
		{
			return MakeResolvedVisualizationViewMode(ActiveVisualizationFamily, ActiveVisualizationMode, FString());
		}
		for (const FSimpleViewportViewModeEntry& Entry : GetSimpleViewportViewModeEntries())
		{
			if (Entry.ViewMode == State.ViewMode) return MakeResolvedSimpleViewMode(Entry);
		}
		FResolvedViewportViewMode Resolved;
		Resolved.ViewMode = State.ViewMode;
		Resolved.Key = ViewModeIndexToStableName(State.ViewMode);
		Resolved.Label = ViewModeIndexToDisplayName(State.ViewMode);
		Resolved.Category = TEXT("view_mode");
		Resolved.bFinalLitComposition = IsFinalLitCompositionViewMode(State.ViewMode);
		return Resolved;
	}

	bool DoesViewportViewModeMatch(const FResolvedViewportViewMode& Requested, const FViewportViewModeState& Actual)
	{
		if (Requested.ViewMode != Actual.ViewMode) return false;
		// A simple mode request changes only the enum, retaining any native family selector.
		if (Requested.VisualizationFamily == EViewportVisualizationFamily::None) return true;
		const FResolvedViewportViewMode ActualMode = DescribeViewportViewModeState(Actual);
		return Requested.VisualizationFamily == ActualMode.VisualizationFamily
			&& Requested.VisualizationMode == ActualMode.VisualizationMode;
	}

	TSharedPtr<FJsonObject> MakeCurrentViewportViewModeJson(FEditorViewportClient* ViewportClient)
	{
		if (!ViewportClient)
		{
			TSharedPtr<FJsonObject> Invalid = MakeShared<FJsonObject>();
			Invalid->SetBoolField(TEXT("valid"), false);
			return Invalid;
		}
		const FViewportViewModeState State = CaptureViewportViewModeState(ViewportClient);
		TSharedPtr<FJsonObject> Obj = MakeResolvedViewportViewModeJson(DescribeViewportViewModeState(State));
		Obj->SetBoolField(TEXT("valid"), true);
		Obj->SetStringField(TEXT("view_mode_display_name"), ViewModeIndexToDisplayName(State.ViewMode));

		TSharedPtr<FJsonObject> Visualization = MakeShared<FJsonObject>();
		Visualization->SetStringField(TEXT("buffer"), State.BufferMode.ToString());
		Visualization->SetStringField(TEXT("nanite"), State.NaniteMode.ToString());
		Visualization->SetStringField(TEXT("lumen"), State.LumenMode.ToString());
		Visualization->SetStringField(TEXT("substrate"), State.SubstrateMode.ToString());
		Visualization->SetStringField(TEXT("groom"), State.GroomMode.ToString());
		Visualization->SetStringField(TEXT("virtual_shadow_map"), State.VirtualShadowMapMode.ToString());
		Visualization->SetStringField(TEXT("virtual_texture"), State.VirtualTextureMode.ToString());
		Visualization->SetStringField(TEXT("actor_coloration"), State.ActorColorationMode.ToString());
		Visualization->SetStringField(TEXT("gpu_skin_cache"), State.GPUSkinCacheMode.ToString());
		Visualization->SetStringField(TEXT("ray_tracing_debug"), State.RayTracingDebugMode.ToString());
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
		for (auto It = ModeMap.CreateConstIterator(); It; ++It)
		{
			const auto& Record = It.Value();
			const FString ModeKey = Record.ModeString.IsEmpty() ? Record.ModeName.ToString() : Record.ModeString;
			const FString Label = Record.ModeText.ToString().IsEmpty() ? ModeKey : Record.ModeText.ToString();
			OutModes.Add(MakeShared<FJsonValueObject>(MakeViewportViewModeCatalogEntry(
				MakeVisualizationModeKey(Family, Record.ModeName),
				VisualizationFamilyToString(Family),
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

	
}
