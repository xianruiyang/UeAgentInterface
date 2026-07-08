#include "Commands/EditorViewport/UeAgentEditorViewportBoundaries.h"

#include "Commands/EditorViewport/UeAgentEditorViewportTypes.h"

namespace UeAgentEditorViewport
{
	FSpecialPreviewBoundary::FSpecialPreviewBoundary()
		: bEditorViewport3DCamera(false)
		, bEditorViewport3DFocus(false)
	{
	}

	static FString NormalizeBoundaryToken(FString Value)
	{
		Value.TrimStartAndEndInline();
		Value.ToLowerInline();
		Value.ReplaceInline(TEXT("-"), TEXT("_"));
		return Value;
	}

	static void SetBoundary(
		FSpecialPreviewBoundary& OutBoundary,
		const TCHAR* Kind,
		const TCHAR* PreviewModel,
		const TCHAR* ErrorCode,
		const TCHAR* RecommendedCommandFamily,
		const TCHAR* Status,
		const TArray<FString>& PlannedCapabilities)
	{
		OutBoundary.Kind = Kind;
		OutBoundary.PreviewModel = PreviewModel;
		OutBoundary.ErrorCode = ErrorCode;
		OutBoundary.RecommendedCommandFamily = RecommendedCommandFamily;
		OutBoundary.Status = Status;
		OutBoundary.PlannedCapabilities = PlannedCapabilities;
	}

	bool TryGetSpecialPreviewBoundaryForAssetType(const FString& InAssetType, FSpecialPreviewBoundary& OutBoundary)
	{
		const FString Type = NormalizeBoundaryToken(InAssetType);
		if (Type.IsEmpty())
		{
			return false;
		}

		if (Type == TEXT("sparse_volume_texture")
			|| Type == TEXT("sparsevolumetexture")
			|| Type == TEXT("usparsevolumetexture")
			|| Type == TEXT("streamable_sparse_volume_texture")
			|| Type == TEXT("streamablesparsevolumetexture")
			|| Type == TEXT("animated_sparse_volume_texture")
			|| Type == TEXT("animatedsparsevolumetexture"))
		{
			SetBoundary(OutBoundary, TEXT("sparse_volume_texture"), TEXT("special_volume_preview"), TEXT("special_sparse_volume_texture_preview_not_3d_editor_viewport"), TEXT("sparse_volume_texture_*"), TEXT("special_preview_unsupported"), { TEXT("get_state"), TEXT("screenshot"), TEXT("frame_or_slice") });
			return true;
		}

		if (Type == TEXT("texture_render_target")
			|| Type == TEXT("texturerendertarget")
			|| Type == TEXT("texture_render_target_2d")
			|| Type == TEXT("texturerendertarget2d")
			|| Type == TEXT("utexturerendertarget2d")
			|| Type == TEXT("canvas_render_target_2d")
			|| Type == TEXT("canvasrendertarget2d")
			|| Type == TEXT("texture_render_target_cube")
			|| Type == TEXT("texturerendertargetcube")
			|| Type == TEXT("texture_render_target_2d_array")
			|| Type == TEXT("texturerendertarget2darray")
			|| Type == TEXT("texture_render_target_volume")
			|| Type == TEXT("texturerendertargetvolume")
			|| Type == TEXT("render_target")
			|| Type == TEXT("render_target_2d"))
		{
			SetBoundary(OutBoundary, TEXT("render_target"), TEXT("2d_or_cubemap_preview"), TEXT("special_render_target_preview_not_3d_editor_viewport"), TEXT("render_target_*"), TEXT("special_preview_unsupported"), { TEXT("get_state"), TEXT("screenshot"), TEXT("mip_or_face_or_slice") });
			return true;
		}

		if (Type == TEXT("media_texture")
			|| Type == TEXT("mediatexture")
			|| Type == TEXT("media_player")
			|| Type == TEXT("mediaplayer")
			|| Type == TEXT("media_source")
			|| Type == TEXT("mediasource")
			|| Type == TEXT("file_media_source")
			|| Type == TEXT("filemediasource")
			|| Type == TEXT("stream_media_source")
			|| Type == TEXT("streammediasource")
			|| Type == TEXT("img_media_source")
			|| Type == TEXT("imgmediasource")
			|| Type == TEXT("platform_media_source")
			|| Type == TEXT("platformmediasource")
			|| Type == TEXT("media_playlist")
			|| Type == TEXT("mediaplaylist"))
		{
			SetBoundary(OutBoundary, TEXT("media"), TEXT("2d_media_preview"), TEXT("special_media_preview_not_3d_editor_viewport"), TEXT("media_*"), TEXT("special_preview_unsupported"), { TEXT("get_state"), TEXT("screenshot"), TEXT("playback_time") });
			return true;
		}

		if (Type == TEXT("paper2d")
			|| Type == TEXT("paper_sprite")
			|| Type == TEXT("papersprite")
			|| Type == TEXT("paper_flipbook")
			|| Type == TEXT("paperflipbook")
			|| Type == TEXT("paper_tileset")
			|| Type == TEXT("papertileset")
			|| Type == TEXT("paper_tile_set")
			|| Type == TEXT("paper_tilemap")
			|| Type == TEXT("papertilemap"))
		{
			SetBoundary(OutBoundary, TEXT("paper2d"), TEXT("2d_sprite_or_flipbook_preview"), TEXT("special_paper2d_preview_not_3d_editor_viewport"), TEXT("paper2d_*"), TEXT("special_preview_unsupported"), { TEXT("get_state"), TEXT("screenshot"), TEXT("frame") });
			return true;
		}

		if (Type == TEXT("subuv_animation")
			|| Type == TEXT("subuvanimation")
			|| Type == TEXT("usubuvanimation"))
		{
			SetBoundary(OutBoundary, TEXT("subuv_animation"), TEXT("2d_subuv_preview"), TEXT("special_subuv_preview_not_3d_editor_viewport"), TEXT("subuv_animation_*"), TEXT("special_preview_unsupported"), { TEXT("get_state"), TEXT("screenshot"), TEXT("frame") });
			return true;
		}

		if (Type == TEXT("texture")
			|| Type == TEXT("utexture")
			|| Type == TEXT("texture_2d")
			|| Type == TEXT("texture2d")
			|| Type == TEXT("utexture2d")
			|| Type == TEXT("texture_cube")
			|| Type == TEXT("texturecube")
			|| Type == TEXT("utexturecube")
			|| Type == TEXT("texture_2d_array")
			|| Type == TEXT("texture2darray")
			|| Type == TEXT("utexture2darray")
			|| Type == TEXT("texture_cube_array")
			|| Type == TEXT("texturecubearray")
			|| Type == TEXT("utexturecubearray")
			|| Type == TEXT("volume_texture")
			|| Type == TEXT("volumetexture")
			|| Type == TEXT("uvolumetexture")
			|| Type == TEXT("virtual_texture")
			|| Type == TEXT("virtualtexture")
			|| Type == TEXT("virtual_texture_2d")
			|| Type == TEXT("virtualtexture2d")
			|| Type == TEXT("runtime_virtual_texture")
			|| Type == TEXT("runtimevirtualtexture")
			|| Type == TEXT("light_profile")
			|| Type == TEXT("lightprofile"))
		{
			SetBoundary(OutBoundary, TEXT("texture"), TEXT("2d_or_cubemap_or_volume_texture_preview"), TEXT("special_texture_preview_not_3d_editor_viewport"), TEXT("texture_*"), TEXT("special_preview_unsupported"), { TEXT("get_state"), TEXT("screenshot"), TEXT("mip_or_face_or_slice") });
			return true;
		}

		return false;
	}

	bool TryGetExcludedEditorWindowBoundaryForAssetType(const FString& InAssetType, FSpecialPreviewBoundary& OutBoundary)
	{
		const FString Type = NormalizeBoundaryToken(InAssetType);
		if (Type == TEXT("widget_blueprint")
			|| Type == TEXT("widgetblueprint")
			|| Type == TEXT("uwidgetblueprint")
			|| Type == TEXT("user_widget")
			|| Type == TEXT("userwidget"))
		{
			SetBoundary(OutBoundary, TEXT("umg_designer"), TEXT("2d_widget_designer_canvas"), TEXT("excluded_umg_designer_not_3d_editor_viewport"), TEXT("umg_*"), TEXT("excluded_editor_window"), { TEXT("widget_screenshot"), TEXT("hierarchy_state") });
			return true;
		}

		if (Type == TEXT("pcg_graph")
			|| Type == TEXT("pcggraph")
			|| Type == TEXT("upcggraph"))
		{
			SetBoundary(OutBoundary, TEXT("pcg_graph"), TEXT("2d_graph_canvas"), TEXT("excluded_pcg_graph_not_3d_editor_viewport"), TEXT("pcg_* or graph/window screenshot"), TEXT("excluded_editor_window"), { TEXT("graph_state"), TEXT("window_screenshot") });
			return true;
		}

		if (Type == TEXT("behavior_tree")
			|| Type == TEXT("behaviortree")
			|| Type == TEXT("ubehaviortree")
			|| Type == TEXT("blackboard_data")
			|| Type == TEXT("blackboarddata"))
		{
			SetBoundary(OutBoundary, TEXT("behavior_tree"), TEXT("2d_behavior_tree_graph"), TEXT("excluded_behavior_tree_not_3d_editor_viewport"), TEXT("ai_* or graph/window screenshot"), TEXT("excluded_editor_window"), { TEXT("graph_state"), TEXT("window_screenshot") });
			return true;
		}

		if (Type == TEXT("data_table")
			|| Type == TEXT("datatable")
			|| Type == TEXT("udatatable"))
		{
			SetBoundary(OutBoundary, TEXT("data_table"), TEXT("2d_table_editor"), TEXT("excluded_data_table_not_3d_editor_viewport"), TEXT("data_table_*"), TEXT("excluded_editor_window"), { TEXT("table_state"), TEXT("window_screenshot") });
			return true;
		}

		if (Type == TEXT("curve")
			|| Type == TEXT("curve_float")
			|| Type == TEXT("curvefloat")
			|| Type == TEXT("curve_vector")
			|| Type == TEXT("curvevector")
			|| Type == TEXT("curve_linear_color")
			|| Type == TEXT("curvelinearcolor"))
		{
			SetBoundary(OutBoundary, TEXT("curve_editor"), TEXT("2d_curve_editor"), TEXT("excluded_curve_editor_not_3d_editor_viewport"), TEXT("curve_* or window screenshot"), TEXT("excluded_editor_window"), { TEXT("curve_state"), TEXT("window_screenshot") });
			return true;
		}

		return false;
	}

	bool TryGetExcludedEditorWindowBoundaryForViewportRole(const FString& InViewportRole, const FString& InAssetType, FSpecialPreviewBoundary& OutBoundary)
	{
		const FString Role = NormalizeBoundaryToken(InViewportRole);
		if (Role.IsEmpty() || Role == TEXT("primary") || Role == TEXT("preview") || Role == TEXT("components"))
		{
			return false;
		}

		FSpecialPreviewBoundary AssetBoundary;
		if ((Role == TEXT("designer") || Role == TEXT("umg_designer") || Role == TEXT("widget_designer"))
			&& TryGetExcludedEditorWindowBoundaryForAssetType(TEXT("widget_blueprint"), AssetBoundary))
		{
			OutBoundary = AssetBoundary;
			return true;
		}

		if (Role == TEXT("graph")
			|| Role == TEXT("node_graph")
			|| Role == TEXT("blueprint_graph")
			|| Role == TEXT("material_graph")
			|| Role == TEXT("niagara_graph")
			|| Role == TEXT("pcg_graph")
			|| Role == TEXT("behavior_tree_graph"))
		{
			if (Role == TEXT("pcg_graph"))
			{
				return TryGetExcludedEditorWindowBoundaryForAssetType(TEXT("pcg_graph"), OutBoundary);
			}
			if (Role == TEXT("behavior_tree_graph"))
			{
				return TryGetExcludedEditorWindowBoundaryForAssetType(TEXT("behavior_tree"), OutBoundary);
			}
			SetBoundary(OutBoundary, TEXT("graph_window"), TEXT("2d_graph_canvas"), TEXT("excluded_graph_window_not_3d_editor_viewport"), TEXT("node_graph_* or window screenshot"), TEXT("excluded_editor_window"), { TEXT("graph_state"), TEXT("window_screenshot") });
			return true;
		}

		if (Role == TEXT("canvas") || Role == TEXT("editor_ui") || Role == TEXT("details") || Role == TEXT("palette"))
		{
			SetBoundary(OutBoundary, TEXT("editor_window"), TEXT("2d_editor_ui"), TEXT("excluded_editor_window_not_3d_editor_viewport"), TEXT("domain-specific command or window screenshot"), TEXT("excluded_editor_window"), { TEXT("window_screenshot") });
			return true;
		}

		return TryGetExcludedEditorWindowBoundaryForAssetType(InAssetType, OutBoundary);
	}

	bool TryGetEditorViewportBoundaryForAssetType(const FString& InAssetType, FSpecialPreviewBoundary& OutBoundary)
	{
		return TryGetSpecialPreviewBoundaryForAssetType(InAssetType, OutBoundary)
			|| TryGetExcludedEditorWindowBoundaryForAssetType(InAssetType, OutBoundary);
	}

	TSharedPtr<FJsonObject> MakeEditorViewportBoundaryJson(const FSpecialPreviewBoundary& Boundary)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("kind"), Boundary.Kind);
		Obj->SetStringField(TEXT("preview_model"), Boundary.PreviewModel);
		Obj->SetBoolField(TEXT("editor_viewport_3d_camera"), Boundary.bEditorViewport3DCamera);
		Obj->SetBoolField(TEXT("editor_viewport_3d_focus"), Boundary.bEditorViewport3DFocus);
		Obj->SetStringField(TEXT("unsupported_reason"), Boundary.ErrorCode);
		Obj->SetStringField(TEXT("recommended_command_family"), Boundary.RecommendedCommandFamily);
		Obj->SetArrayField(TEXT("planned_capabilities"), MakeStringArrayJson(Boundary.PlannedCapabilities));
		return Obj;
	}

	void ApplyEditorViewportBoundaryToCandidate(TSharedPtr<FJsonObject> Candidate, const FSpecialPreviewBoundary& Boundary)
	{
		if (!Candidate.IsValid())
		{
			return;
		}
		Candidate->SetStringField(TEXT("status"), Boundary.Status.IsEmpty() ? TEXT("unsupported_asset_editor_viewport") : Boundary.Status);
		Candidate->SetStringField(TEXT("unsupported_reason"), Boundary.ErrorCode);
		const TCHAR* FieldName = Boundary.Status == TEXT("excluded_editor_window") ? TEXT("excluded_window") : TEXT("special_preview");
		Candidate->SetObjectField(FieldName, MakeEditorViewportBoundaryJson(Boundary));
	}
}
