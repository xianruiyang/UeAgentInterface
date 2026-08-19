#include "UeAgentHttpServer_Level.h"

#include "UeAgentHttpServer_LevelOps.h"

#include "Commands/EditorViewport/UeAgentEditorViewportSequenceFrameOptions.h"
#include "Commands/EditorViewport/UeAgentEditorViewportLegacy.h"
#include "Module/UeAgentInterfaceLogger.h"
#include "Utilities/UeAgentJsonDiagnostics.h"

#include "BufferVisualizationData.h"
#include "Components/DirectionalLightComponent.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/PlanarReflection.h"
#include "EngineUtils.h"
#include "GPUSkinCacheVisualizationData.h"
#include "GroomVisualizationData.h"
#include "HAL/IConsoleManager.h"
#include "LevelEditorViewport.h"
#include "LumenVisualizationData.h"
#include "NaniteVisualizationData.h"
#include "RayTracingVisualizationData.h"
#include "SceneView.h"
#include "Selection.h"
#include "SubstrateVisualizationData.h"
#include "VirtualShadowMapVisualizationData.h"

namespace
{
	static int32 GetConsoleVariableIntForWarnings(const TCHAR* Name, bool& bFound)
	{
		bFound = false;
		IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name);
		if (!Variable)
		{
			return 0;
		}

		bFound = true;
		return Variable->GetInt();
	}

	static bool IsActorVisibleForViewportWarning(const AActor* Actor)
	{
		return Actor
			&& !Actor->IsActorBeingDestroyed()
			&& !Actor->IsHidden()
			&& !Actor->IsHiddenEd()
			&& !Actor->IsTemporarilyHiddenInEditor();
	}

	static bool IsDirectionalLightRelevantForForwardWarning(const UDirectionalLightComponent* Component)
	{
		const AActor* Owner = Component ? Component->GetOwner() : nullptr;
		return Component
			&& IsActorVisibleForViewportWarning(Owner)
			&& Component->IsRegistered()
			&& Component->IsVisible()
			&& Component->bAffectsWorld
			&& Component->Intensity > 0.0f;
	}

	static TSharedPtr<FJsonObject> MakeViewportWarningJson(
		const FString& Code,
		const FString& Source,
		const FText& Text,
		const FString& StableEnglishText,
		const FLinearColor& DisplayColor,
		bool bDisplayExpected,
		const TSharedPtr<FJsonObject>& Details)
	{
		TSharedPtr<FJsonObject> Warning = MakeShared<FJsonObject>();
		Warning->SetStringField(TEXT("code"), Code);
		Warning->SetStringField(TEXT("severity"), TEXT("warning"));
		Warning->SetStringField(TEXT("source"), Source);
		Warning->SetStringField(TEXT("text"), Text.ToString());
		Warning->SetStringField(TEXT("stable_english_text"), StableEnglishText);
		Warning->SetBoolField(TEXT("display_expected"), bDisplayExpected);
		Warning->SetStringField(TEXT("display_color"), DisplayColor.ToFColor(true).ToHex());
		if (Details.IsValid())
		{
			Warning->SetObjectField(TEXT("details"), Details);
		}
		return Warning;
	}
}

bool UeAgentLevelOps::BuildViewportWarnings(
	UWorld* World,
	const bool bIncludeSuppressed,
	TSharedPtr<FJsonObject>& OutData,
	FString& OutError)
{
	if (!World)
	{
		OutError = TEXT("no_editor_world");
		return false;
	}
	if (!OutData.IsValid())
	{
		OutData = MakeShared<FJsonObject>();
	}

	bool bClipPlaneCVarFound = false;
	const int32 ClipPlaneCVarValue = GetConsoleVariableIntForWarnings(TEXT("r.AllowGlobalClipPlane"), bClipPlaneCVarFound);

	const bool bScreenMessagesEnabled = GAreScreenMessagesEnabled;
	const bool bMapWarningsSuppressed = GEngine ? GEngine->bSuppressMapWarnings : false;
	const bool bDisplayGateOpen = bScreenMessagesEnabled && !bMapWarningsSuppressed;

	TArray<TSharedPtr<FJsonValue>> Warnings;
	TArray<TSharedPtr<FJsonValue>> SuppressedWarnings;

	auto AddWarning = [&](const TSharedPtr<FJsonObject>& Warning)
	{
		if (!Warning.IsValid())
		{
			return;
		}
		if (Warning->GetBoolField(TEXT("display_expected")) || bIncludeSuppressed)
		{
			Warnings.Add(MakeShared<FJsonValueObject>(Warning));
		}
		else
		{
			SuppressedWarnings.Add(MakeShared<FJsonValueObject>(Warning));
		}
	};

	int32 PlanarReflectionCount = 0;
	for (TActorIterator<APlanarReflection> It(World); It; ++It)
	{
		if (IsActorVisibleForViewportWarning(*It))
		{
			PlanarReflectionCount++;
		}
	}

	if (PlanarReflectionCount > 0 && bClipPlaneCVarFound && ClipPlaneCVarValue == 0)
	{
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetNumberField(TEXT("planar_reflection_count"), PlanarReflectionCount);
		Details->SetStringField(TEXT("console_variable"), TEXT("r.AllowGlobalClipPlane"));
		Details->SetNumberField(TEXT("console_variable_value"), ClipPlaneCVarValue);
		Details->SetStringField(TEXT("renderer_source"), TEXT("FSceneRenderer::OnRenderFinish"));
		AddWarning(MakeViewportWarningJson(
			TEXT("renderer.no_global_clip_plane"),
			TEXT("renderer_condition"),
			NSLOCTEXT("Renderer", "NoGlobalClipPlane", "PLANAR REFLECTION REQUIRES GLOBAL CLIP PLANE PROJECT SETTING ENABLED TO WORK PROPERLY"),
			TEXT("PLANAR REFLECTION REQUIRES GLOBAL CLIP PLANE PROJECT SETTING ENABLED TO WORK PROPERLY"),
			FLinearColor::Red,
			bDisplayGateOpen,
			Details));
	}

	int32 HighestForwardPriority = TNumericLimits<int32>::Lowest();
	TArray<TSharedPtr<FJsonValue>> ActiveDirectionalLights;
	TArray<TSharedPtr<FJsonValue>> ConflictingDirectionalLights;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!IsActorVisibleForViewportWarning(Actor))
		{
			continue;
		}

		TInlineComponentArray<UDirectionalLightComponent*> Components(Actor);
		for (UDirectionalLightComponent* Component : Components)
		{
			if (!IsDirectionalLightRelevantForForwardWarning(Component))
			{
				continue;
			}

			HighestForwardPriority = FMath::Max(HighestForwardPriority, Component->ForwardShadingPriority);

			TSharedPtr<FJsonObject> LightJson = MakeShared<FJsonObject>();
			LightJson->SetStringField(TEXT("actor_name"), Actor->GetName());
			LightJson->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
			LightJson->SetStringField(TEXT("component_name"), Component->GetName());
			LightJson->SetNumberField(TEXT("forward_shading_priority"), Component->ForwardShadingPriority);
			LightJson->SetNumberField(TEXT("intensity"), Component->Intensity);
			LightJson->SetBoolField(TEXT("registered"), Component->IsRegistered());
			LightJson->SetBoolField(TEXT("visible"), Component->IsVisible());
			LightJson->SetBoolField(TEXT("affects_world"), Component->bAffectsWorld != 0);
			ActiveDirectionalLights.Add(MakeShared<FJsonValueObject>(LightJson));
		}
	}

	if (ActiveDirectionalLights.Num() > 0)
	{
		for (const TSharedPtr<FJsonValue>& LightValue : ActiveDirectionalLights)
		{
			const TSharedPtr<FJsonObject>* LightObj = nullptr;
			if (LightValue.IsValid()
				&& LightValue->TryGetObject(LightObj)
				&& LightObj
				&& LightObj->IsValid()
				&& (*LightObj)->GetIntegerField(TEXT("forward_shading_priority")) == HighestForwardPriority)
			{
				ConflictingDirectionalLights.Add(LightValue);
			}
		}
	}

	if (ConflictingDirectionalLights.Num() > 1)
	{
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetNumberField(TEXT("active_directional_light_count"), ActiveDirectionalLights.Num());
		Details->SetNumberField(TEXT("conflicting_directional_light_count"), ConflictingDirectionalLights.Num());
		Details->SetNumberField(TEXT("highest_forward_shading_priority"), HighestForwardPriority);
		Details->SetArrayField(TEXT("conflicting_directional_lights"), ConflictingDirectionalLights);
		Details->SetStringField(TEXT("renderer_source"), TEXT("LightGridInjection.cpp OnGetOnScreenMessages"));
		AddWarning(MakeViewportWarningJson(
			TEXT("renderer.multiple_directional_lights_forward_shading"),
			TEXT("renderer_condition"),
			NSLOCTEXT("Renderer", "MultipleDirLightsConflictForForwardShading", "Multiple directional lights are competing to be the single one used for forward shading, translucent, water or volumetric fog. Please adjust their ForwardShadingPriority.\nAs a fallback, the main directional light will be selected based on overall brightness."),
			TEXT("Multiple directional lights are competing to be the single one used for forward shading, translucent, water or volumetric fog. Please adjust their ForwardShadingPriority.\nAs a fallback, the main directional light will be selected based on overall brightness."),
			FLinearColor(1.0f, 0.45f, 0.0f, 1.0f),
			bDisplayGateOpen,
			Details));
	}

	TSharedPtr<FJsonObject> WorldJson = MakeShared<FJsonObject>();
	WorldJson->SetStringField(TEXT("map_name"), World->GetMapName());
	WorldJson->SetStringField(TEXT("package_name"), World->GetOutermost() ? World->GetOutermost()->GetName() : FString());
	WorldJson->SetBoolField(TEXT("dirty"), World->GetOutermost() ? World->GetOutermost()->IsDirty() : false);

	TSharedPtr<FJsonObject> GatesJson = MakeShared<FJsonObject>();
	GatesJson->SetBoolField(TEXT("screen_messages_enabled"), bScreenMessagesEnabled);
	GatesJson->SetBoolField(TEXT("map_warnings_suppressed"), bMapWarningsSuppressed);
	GatesJson->SetBoolField(TEXT("display_gate_open"), bDisplayGateOpen);
	GatesJson->SetBoolField(TEXT("include_suppressed"), bIncludeSuppressed);

	TSharedPtr<FJsonObject> RendererProbeJson = MakeShared<FJsonObject>();
	RendererProbeJson->SetBoolField(TEXT("clip_plane_cvar_found"), bClipPlaneCVarFound);
	RendererProbeJson->SetNumberField(TEXT("r.AllowGlobalClipPlane"), ClipPlaneCVarValue);
	RendererProbeJson->SetNumberField(TEXT("planar_reflection_count"), PlanarReflectionCount);
	RendererProbeJson->SetNumberField(TEXT("active_directional_light_count"), ActiveDirectionalLights.Num());
	RendererProbeJson->SetNumberField(TEXT("highest_forward_shading_priority"), ActiveDirectionalLights.Num() > 0 ? HighestForwardPriority : 0);
	RendererProbeJson->SetArrayField(TEXT("active_directional_lights"), ActiveDirectionalLights);

	OutData->SetStringField(TEXT("schema"), TEXT("uai.viewport_warnings.v1"));
	OutData->SetObjectField(TEXT("world"), WorldJson);
	OutData->SetObjectField(TEXT("display_gates"), GatesJson);
	OutData->SetObjectField(TEXT("renderer_probe"), RendererProbeJson);
	OutData->SetNumberField(TEXT("warning_count"), Warnings.Num());
	OutData->SetNumberField(TEXT("suppressed_warning_count"), SuppressedWarnings.Num());
	OutData->SetArrayField(TEXT("warnings"), Warnings);
	OutData->SetArrayField(TEXT("suppressed_warnings"), SuppressedWarnings);
	return true;
}

bool FUeAgentHttpServer::CmdViewportGetWarnings(
	const FUeAgentRequestContext& Ctx,
	TSharedPtr<FJsonObject>& OutData,
	FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}
	bool bIncludeSuppressed = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_suppressed"), bIncludeSuppressed);
	return UeAgentLevelOps::BuildViewportWarnings(
		World, bIncludeSuppressed, OutData, OutError);
}

bool FUeAgentHttpServer::CmdViewportGetCamera(TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FUeAgentRequestContext CompatCtx;
	CompatCtx.Command = TEXT("viewport_get_camera");
	CompatCtx.RequestId = TEXT("viewport_get_camera_compat");
	CompatCtx.Params = MakeShared<FJsonObject>();
	if (!UeAgentEditorViewportLegacy::ExecuteCameraGet(*this, CompatCtx, UeAgentEditorViewportLegacy::MakeLevelTarget(), OutData, OutError))
	{
		return false;
	}
	FString SelectionReason;
	if (FLevelEditorViewportClient* LVC = UeAgentLevelOps::GetPreferredLevelViewportClient(&SelectionReason))
	{
		OutData->SetObjectField(TEXT("viewport"), UeAgentLevelOps::MakeViewportClientJson(LVC, SelectionReason));
	}
	return true;
}

bool FUeAgentHttpServer::CmdViewportSetCamera(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (!UeAgentEditorViewportLegacy::ExecuteCameraSet(*this, Ctx, UeAgentEditorViewportLegacy::MakeLevelTarget(), OutData, OutError))
	{
		return false;
	}
	FString SelectionReason;
	if (FLevelEditorViewportClient* LVC = UeAgentLevelOps::GetPreferredLevelViewportClient(&SelectionReason))
	{
		OutData->SetObjectField(TEXT("viewport"), UeAgentLevelOps::MakeViewportClientJson(LVC, SelectionReason));
	}
	return true;
}

bool FUeAgentHttpServer::CmdViewportSetRealtime(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FLevelEditorViewportClient* LVC = UeAgentLevelOps::GetPreferredLevelViewportClient();
	if (!LVC)
	{
		OutError = TEXT("no_level_editor_viewport");
		return false;
	}

	bool bRealtime = true;
	if (!JsonTryGetBool(Ctx.Params, TEXT("realtime"), bRealtime)
		&& !JsonTryGetBool(Ctx.Params, TEXT("enabled"), bRealtime))
	{
		OutError = TEXT("missing_realtime");
		return false;
	}

	bool bStoreCurrentValue = false;
	JsonTryGetBool(Ctx.Params, TEXT("store_current_value"), bStoreCurrentValue);

	TArray<FLevelEditorViewportClient*> TargetClients;
	if (GEditor)
	{
		for (FLevelEditorViewportClient* ViewportClient : GEditor->GetLevelViewportClients())
		{
			if (ViewportClient && ViewportClient->Viewport)
			{
				TargetClients.AddUnique(ViewportClient);
			}
		}
	}
	if (TargetClients.Num() == 0)
	{
		TargetClients.Add(LVC);
	}

	const FText RealtimeOverrideSystem = NSLOCTEXT("UeAgentInterface", "RealtimeOverrideSystem", "UeAgentInterface");
	int32 MatchedRealtimeClients = 0;
	for (FLevelEditorViewportClient* ViewportClient : TargetClients)
	{
		if (!ViewportClient)
		{
			continue;
		}

		if (ViewportClient->HasRealtimeOverride(RealtimeOverrideSystem))
		{
			ViewportClient->RemoveRealtimeOverride(RealtimeOverrideSystem, false);
		}
		ViewportClient->SetRealtime(bRealtime);
		ViewportClient->AddRealtimeOverride(bRealtime, RealtimeOverrideSystem);
		ViewportClient->Invalidate();
		if (ViewportClient->Viewport)
		{
			ViewportClient->Viewport->Invalidate();
		}

		if (ViewportClient->IsRealtime() == bRealtime)
		{
			++MatchedRealtimeClients;
		}

		FUeAgentInterfaceLogger::Log(
			TEXT("ViewportSetRealtime: client=%p requested=%s applied=%s store_current_value=%s"),
			ViewportClient,
			bRealtime ? TEXT("true") : TEXT("false"),
			ViewportClient->IsRealtime() ? TEXT("true") : TEXT("false"),
			bStoreCurrentValue ? TEXT("true") : TEXT("false"));
	}

	OutData->SetBoolField(TEXT("realtime"), LVC->IsRealtime());
	OutData->SetNumberField(TEXT("target_client_count"), TargetClients.Num());
	OutData->SetNumberField(TEXT("matched_realtime_client_count"), MatchedRealtimeClients);
	OutData->SetBoolField(TEXT("stored_value_updated"), false);
	OutData->SetBoolField(TEXT("store_current_value_ignored"), bStoreCurrentValue);
	OutData->SetObjectField(TEXT("location"), VecToJson(LVC->GetViewLocation()));
	OutData->SetObjectField(TEXT("rotation"), RotToJson(LVC->GetViewRotation()));
	OutData->SetNumberField(TEXT("fov"), LVC->ViewFOV);
	return true;
}

bool FUeAgentHttpServer::CmdViewportSetGameView(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FLevelEditorViewportClient* LVC = UeAgentLevelOps::GetPreferredLevelViewportClient();
	if (!LVC)
	{
		OutError = TEXT("no_level_editor_viewport");
		return false;
	}

	bool bGameView = true;
	if (!JsonTryGetBool(Ctx.Params, TEXT("game_view"), bGameView))
	{
		OutError = TEXT("missing_game_view");
		return false;
	}

	TArray<FLevelEditorViewportClient*> TargetClients;
	if (GEditor)
	{
		for (FLevelEditorViewportClient* ViewportClient : GEditor->GetLevelViewportClients())
		{
			if (ViewportClient && ViewportClient->Viewport)
			{
				TargetClients.AddUnique(ViewportClient);
			}
		}
	}
	if (TargetClients.Num() == 0)
	{
		TargetClients.Add(LVC);
	}

	int32 MatchedGameViewClients = 0;
	bool bRestoreEditorWidgets = !bGameView;
	JsonTryGetBool(Ctx.Params, TEXT("restore_editor_widgets"), bRestoreEditorWidgets);
	for (FLevelEditorViewportClient* ViewportClient : TargetClients)
	{
		if (!ViewportClient)
		{
			continue;
		}

		ViewportClient->SetGameView(bGameView);
		if (!bGameView && bRestoreEditorWidgets)
		{
			UeAgentLevelOps::RestoreEditorInteractionState(
				ViewportClient,
				false,
				true,
				true,
				true,
				TEXT("translate_if_none"));
		}
		ViewportClient->Invalidate();
		if (ViewportClient->Viewport)
		{
			ViewportClient->Viewport->Invalidate();
		}

		if (ViewportClient->IsInGameView() == bGameView)
		{
			++MatchedGameViewClients;
		}
	}

	OutData->SetBoolField(TEXT("game_view"), LVC->IsInGameView());
	OutData->SetNumberField(TEXT("target_client_count"), TargetClients.Num());
	OutData->SetNumberField(TEXT("matched_game_view_client_count"), MatchedGameViewClients);
	OutData->SetBoolField(TEXT("restore_editor_widgets"), bRestoreEditorWidgets);
	OutData->SetBoolField(TEXT("realtime"), LVC->IsRealtime());
	OutData->SetObjectField(TEXT("location"), VecToJson(LVC->GetViewLocation()));
	OutData->SetObjectField(TEXT("rotation"), RotToJson(LVC->GetViewRotation()));
	OutData->SetNumberField(TEXT("fov"), LVC->ViewFOV);
	OutData->SetObjectField(TEXT("viewport"), UeAgentLevelOps::MakeViewportClientJson(LVC, TEXT("preferred_after_game_view")));
	return true;
}

bool FUeAgentHttpServer::CmdViewportGetEditorInteractionState(TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString SelectionReason;
	FLevelEditorViewportClient* LVC = UeAgentLevelOps::GetPreferredLevelViewportClient(&SelectionReason);
	if (!LVC)
	{
		OutError = TEXT("no_level_editor_viewport");
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> ViewportValues;
	if (GEditor)
	{
		int32 Index = 0;
		for (FLevelEditorViewportClient* ViewportClient : GEditor->GetLevelViewportClients())
		{
			TSharedPtr<FJsonObject> ViewportJson = UeAgentLevelOps::MakeViewportClientJson(ViewportClient, FString::Printf(TEXT("level_viewport_client_%d"), Index));
			ViewportValues.Add(MakeShared<FJsonValueObject>(ViewportJson));
			++Index;
		}
	}

	int32 SelectedActorCount = 0;
	if (GEditor)
	{
		if (USelection* SelectedActors = GEditor->GetSelectedActors())
		{
			for (FSelectionIterator It(*SelectedActors); It; ++It)
			{
				if (Cast<AActor>(*It))
				{
					++SelectedActorCount;
				}
			}
		}
	}

	OutData = MakeShared<FJsonObject>();
	OutData->SetObjectField(TEXT("viewport"), UeAgentLevelOps::MakeViewportClientJson(LVC, SelectionReason));
	OutData->SetArrayField(TEXT("viewports"), ViewportValues);
	OutData->SetNumberField(TEXT("viewport_count"), ViewportValues.Num());
	OutData->SetNumberField(TEXT("selected_actor_count"), SelectedActorCount);
	return true;
}

bool FUeAgentHttpServer::CmdViewportRestoreEditorInteractionState(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString SelectionReason;
	FLevelEditorViewportClient* LVC = UeAgentLevelOps::GetPreferredLevelViewportClient(&SelectionReason);
	if (!LVC)
	{
		OutError = TEXT("no_level_editor_viewport");
		return false;
	}

	bool bAllViewports = true;
	JsonTryGetBool(Ctx.Params, TEXT("all_viewports"), bAllViewports);
	bool bGameView = false;
	JsonTryGetBool(Ctx.Params, TEXT("game_view"), bGameView);
	bool bModeWidgets = true;
	JsonTryGetBool(Ctx.Params, TEXT("mode_widgets"), bModeWidgets);
	bool bSelection = true;
	JsonTryGetBool(Ctx.Params, TEXT("selection"), bSelection);
	bool bPivot = true;
	JsonTryGetBool(Ctx.Params, TEXT("pivot"), bPivot);
	bool bRedraw = true;
	JsonTryGetBool(Ctx.Params, TEXT("redraw"), bRedraw);
	FString WidgetModePolicy = TEXT("translate_if_none");
	JsonTryGetString(Ctx.Params, TEXT("widget_mode"), WidgetModePolicy);
	WidgetModePolicy.TrimStartAndEndInline();
	if (WidgetModePolicy.IsEmpty())
	{
		WidgetModePolicy = TEXT("translate_if_none");
	}

	TArray<FLevelEditorViewportClient*> TargetClients;
	if (bAllViewports && GEditor)
	{
		for (FLevelEditorViewportClient* ViewportClient : GEditor->GetLevelViewportClients())
		{
			if (ViewportClient && ViewportClient->Viewport)
			{
				TargetClients.AddUnique(ViewportClient);
			}
		}
	}
	if (TargetClients.Num() == 0)
	{
		TargetClients.Add(LVC);
	}

	for (FLevelEditorViewportClient* ViewportClient : TargetClients)
	{
		UeAgentLevelOps::RestoreEditorInteractionState(ViewportClient, bGameView, bModeWidgets, bSelection, bPivot, WidgetModePolicy);
	}
	if (bRedraw && GEditor)
	{
		GEditor->RedrawLevelEditingViewports(true);
	}

	TArray<TSharedPtr<FJsonValue>> ViewportValues;
	for (FLevelEditorViewportClient* ViewportClient : TargetClients)
	{
		ViewportValues.Add(MakeShared<FJsonValueObject>(UeAgentLevelOps::MakeViewportClientJson(ViewportClient, TEXT("restored"))));
	}

	OutData = MakeShared<FJsonObject>();
	OutData->SetBoolField(TEXT("all_viewports"), bAllViewports);
	OutData->SetBoolField(TEXT("game_view"), bGameView);
	OutData->SetBoolField(TEXT("mode_widgets"), bModeWidgets);
	OutData->SetBoolField(TEXT("selection"), bSelection);
	OutData->SetBoolField(TEXT("pivot"), bPivot);
	OutData->SetStringField(TEXT("widget_mode"), WidgetModePolicy);
	OutData->SetNumberField(TEXT("target_client_count"), TargetClients.Num());
	OutData->SetObjectField(TEXT("viewport"), UeAgentLevelOps::MakeViewportClientJson(LVC, SelectionReason));
	OutData->SetArrayField(TEXT("viewports"), ViewportValues);
	return true;
}

bool FUeAgentHttpServer::CmdViewportListViewModes(TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString SelectionReason;
	FLevelEditorViewportClient* LVC = UeAgentLevelOps::GetPreferredLevelViewportClient(&SelectionReason);

	OutData = UeAgentLevelOps::MakeViewportViewModeCatalogJson();
	if (LVC)
	{
		OutData->SetObjectField(TEXT("viewport"), UeAgentLevelOps::MakeViewportClientJson(LVC, SelectionReason));
	}
	else
	{
		TSharedPtr<FJsonObject> ViewportJson = MakeShared<FJsonObject>();
		ViewportJson->SetBoolField(TEXT("valid"), false);
		ViewportJson->SetStringField(TEXT("selection_reason"), SelectionReason);
		OutData->SetObjectField(TEXT("viewport"), ViewportJson);
	}
	return true;
}

bool FUeAgentHttpServer::CmdViewportFocusActor(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString Id;
	if (!JsonTryGetString(Ctx.Params, TEXT("id"), Id) || Id.IsEmpty())
	{
		OutError = TEXT("missing_id");
		return false;
	}

	TSharedPtr<FJsonObject> Focus = MakeShared<FJsonObject>();
	Focus->SetStringField(TEXT("mode"), TEXT("actor"));
	Focus->SetStringField(TEXT("id"), Id);
	double Padding = 1.1;
	if (JsonTryGetNumber(Ctx.Params, TEXT("padding"), Padding))
	{
		Focus->SetNumberField(TEXT("padding"), Padding);
	}
	bool bInstant = true;
	if (JsonTryGetBool(Ctx.Params, TEXT("instant"), bInstant))
	{
		Focus->SetBoolField(TEXT("instant"), bInstant);
	}
	return UeAgentEditorViewportLegacy::ExecuteFocus(*this, Ctx, UeAgentEditorViewportLegacy::MakeLevelTarget(), Focus, OutData, OutError);
}

bool FUeAgentHttpServer::CmdViewportFocusActorSafe(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	// `viewport_focus_actor` uses the editor's FocusViewportOnBox, which is convenient but not collision-aware.
	// This "safe" variant delegates to `viewport_frame_actor` with better defaults for indoor whitebox validation.
	TSharedPtr<FJsonObject> ParamsCopy = MakeShared<FJsonObject>();
	if (Ctx.Params.IsValid())
	{
		ParamsCopy->Values = Ctx.Params->Values;
	}

	if (!ParamsCopy->HasField(TEXT("collision_aware")))
	{
		ParamsCopy->SetBoolField(TEXT("collision_aware"), true);
	}
	if (!ParamsCopy->HasField(TEXT("look_at")))
	{
		ParamsCopy->SetBoolField(TEXT("look_at"), true);
	}
	if (!ParamsCopy->HasField(TEXT("auto_fallback")))
	{
		ParamsCopy->SetBoolField(TEXT("auto_fallback"), true);
	}

	FUeAgentRequestContext NewCtx = Ctx;
	NewCtx.Params = ParamsCopy;
	return CmdViewportFrameActor(NewCtx, OutData, OutError);
}

bool FUeAgentHttpServer::CmdViewportFrameActor(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString Id;
	if (!JsonTryGetString(Ctx.Params, TEXT("id"), Id) || Id.IsEmpty())
	{
		OutError = TEXT("missing_id");
		return false;
	}

	TSharedPtr<FJsonObject> Focus = MakeShared<FJsonObject>();
	if (Ctx.Params.IsValid())
	{
		Focus->Values = Ctx.Params->Values;
	}
	Focus->SetStringField(TEXT("mode"), TEXT("actor"));
	Focus->SetStringField(TEXT("id"), Id);
	return UeAgentEditorViewportLegacy::ExecuteFocus(*this, Ctx, UeAgentEditorViewportLegacy::MakeLevelTarget(), Focus, OutData, OutError);
}

bool FUeAgentHttpServer::CmdViewportFrameActorCore(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (!OutData.IsValid())
	{
		OutData = MakeShared<FJsonObject>();
	}

	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	FString Id;
	if (!JsonTryGetString(Ctx.Params, TEXT("id"), Id) || Id.IsEmpty())
	{
		OutError = TEXT("missing_id");
		return false;
	}

	AActor* Actor = FindActorByNameOrLabel(World, Id);
	if (!Actor)
	{
		OutError = TEXT("actor_not_found");
		return false;
	}

	FString SelectionReason;
	FLevelEditorViewportClient* LVC = UeAgentLevelOps::GetPreferredLevelViewportClient(&SelectionReason);
	if (!LVC)
	{
		OutError = TEXT("no_level_editor_viewport");
		return false;
	}

	FBox Bounds(EForceInit::ForceInit);
	if (!UeAgentLevelOps::GetActorBoundsBox(Actor, Bounds, OutError))
	{
		return false;
	}

	double Padding = 1.1;
	JsonTryGetNumber(Ctx.Params, TEXT("padding"), Padding);

	const FVector PreviousLocation = LVC->GetViewLocation();
	const FRotator PreviousRotation = LVC->GetViewRotation();
	FVector DesiredLocation = PreviousLocation;
	FVector NewLocation = PreviousLocation;
	if (!UeAgentLevelOps::ComputeViewportLocationToFrameBounds(LVC, Bounds, PreviousRotation, Padding, NewLocation, OutError))
	{
		return false;
	}

	DesiredLocation = NewLocation;

	bool bCollisionAware = false;
	JsonTryGetBool(Ctx.Params, TEXT("collision_aware"), bCollisionAware);

	bool bLookAt = false;
	JsonTryGetBool(Ctx.Params, TEXT("look_at"), bLookAt);

	bool bCollisionAdjusted = false;
	bool bFallbackUsed = false;
	int32 FallbackIndex = 0;
	FVector FallbackOffset = FVector::ZeroVector;
	TSharedPtr<FJsonObject> CollisionTrace;
	if (bCollisionAware)
	{
		double SafetyOffsetCm = 15.0;
		JsonTryGetNumber(Ctx.Params, TEXT("safety_offset_cm"), SafetyOffsetCm);
		SafetyOffsetCm = FMath::Clamp(SafetyOffsetCm, 0.0, 10000.0);

		ECollisionChannel TraceChannel = ECC_Visibility;
		if (!UeAgentLevelOps::ResolveTraceChannel(Ctx.Params, TraceChannel))
		{
			OutError = TEXT("unsupported_trace_channel");
			return false;
		}

		bool bTraceComplex = false;
		JsonTryGetBool(Ctx.Params, TEXT("trace_complex"), bTraceComplex);

		FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(UeAgentViewportFrameActorCollision), bTraceComplex);
		QueryParams.AddIgnoredActor(Actor);

		bool bAutoFallback = false;
		JsonTryGetBool(Ctx.Params, TEXT("auto_fallback"), bAutoFallback);

		double FallbackStepCm = 200.0;
		JsonTryGetNumber(Ctx.Params, TEXT("fallback_step_cm"), FallbackStepCm);
		FallbackStepCm = FMath::Clamp(FallbackStepCm, 0.0, 10000.0);

		TArray<FVector> CandidateOffsets;
		CandidateOffsets.Reserve(8);
		CandidateOffsets.Add(FVector::ZeroVector);

		const TArray<TSharedPtr<FJsonValue>>* ExplicitOffsets = nullptr;
		if (Ctx.Params.IsValid() && Ctx.Params->TryGetArrayField(TEXT("fallback_offsets_cm"), ExplicitOffsets) && ExplicitOffsets)
		{
			for (const TSharedPtr<FJsonValue>& Value : *ExplicitOffsets)
			{
				if (!Value.IsValid())
				{
					continue;
				}

				FVector Offset = FVector::ZeroVector;
				if (UeAgentJsonDiagnostics::TryReadVectorValue(Value, Offset) && !Offset.IsNearlyZero())
				{
					CandidateOffsets.Add(Offset);
				}
			}
		}

		const FVector Target = Bounds.GetCenter();
		if (bAutoFallback && CandidateOffsets.Num() <= 1 && FallbackStepCm > KINDA_SMALL_NUMBER)
		{
			FVector BaseDir = (DesiredLocation - Target).GetSafeNormal();
			if (BaseDir.IsNearlyZero(KINDA_SMALL_NUMBER))
			{
				BaseDir = (PreviousLocation - Target).GetSafeNormal();
			}
			if (BaseDir.IsNearlyZero(KINDA_SMALL_NUMBER))
			{
				BaseDir = FVector::BackwardVector;
			}

			const FVector WorldUp = FVector::UpVector;
			FVector Right = FVector::CrossProduct(WorldUp, BaseDir).GetSafeNormal();
			if (Right.IsNearlyZero(KINDA_SMALL_NUMBER))
			{
				Right = FVector::CrossProduct(FVector::RightVector, BaseDir).GetSafeNormal();
			}
			FVector Up = FVector::CrossProduct(BaseDir, Right).GetSafeNormal();
			if (Up.IsNearlyZero(KINDA_SMALL_NUMBER))
			{
				Up = WorldUp;
			}

			CandidateOffsets.Add(Right * (float)FallbackStepCm);
			CandidateOffsets.Add(-Right * (float)FallbackStepCm);
			CandidateOffsets.Add(Up * (float)FallbackStepCm);
			CandidateOffsets.Add(-Up * (float)FallbackStepCm);
			CandidateOffsets.Add(BaseDir * (float)FallbackStepCm);
			CandidateOffsets.Add(-BaseDir * (float)FallbackStepCm);
		}

		struct FCollisionCandidate
		{
			int32 Index = 0;
			FVector Offset = FVector::ZeroVector;
			FVector Desired = FVector::ZeroVector;
			FVector Location = FVector::ZeroVector;
			bool bHit = false;
			bool bAdjusted = false;
			FHitResult Hit;
			double Score = 0.0;
		};

		FCollisionCandidate Best;
		Best.Score = -TNumericLimits<double>::Max();

		for (int32 Index = 0; Index < CandidateOffsets.Num(); ++Index)
		{
			const FVector Offset = CandidateOffsets[Index];
			const FVector CandidateDesired = DesiredLocation + Offset;

			FCollisionCandidate Candidate;
			Candidate.Index = Index;
			Candidate.Offset = Offset;
			Candidate.Desired = CandidateDesired;

			const bool bHit = World->LineTraceSingleByChannel(Candidate.Hit, Target, CandidateDesired, TraceChannel, QueryParams);
			Candidate.bHit = bHit;

			if (bHit && Candidate.Hit.bBlockingHit)
			{
				FVector Dir = (CandidateDesired - Target).GetSafeNormal();
				if (Dir.IsNearlyZero(KINDA_SMALL_NUMBER))
				{
					Dir = (DesiredLocation - Target).GetSafeNormal();
				}
				if (Dir.IsNearlyZero(KINDA_SMALL_NUMBER))
				{
					Dir = FVector::BackwardVector;
				}

				Candidate.Location = Candidate.Hit.ImpactPoint - (Dir * (float)SafetyOffsetCm);
				Candidate.bAdjusted = true;
			}
			else
			{
				Candidate.Location = CandidateDesired;
			}

			const double Distance = FVector::Dist(Target, Candidate.Location);
			Candidate.Score = Distance - (Candidate.bAdjusted ? 0.01 : 0.0);
			if (Candidate.Score > Best.Score)
			{
				Best = Candidate;
			}
		}

		NewLocation = Best.Location;
		bCollisionAdjusted = Best.bAdjusted;
		bFallbackUsed = (Best.Index != 0);
		FallbackIndex = Best.Index;
		FallbackOffset = Best.Offset;

		CollisionTrace = MakeShared<FJsonObject>();
		CollisionTrace->SetObjectField(TEXT("target"), VecToJson(Target));
		CollisionTrace->SetObjectField(TEXT("desired_location"), VecToJson(DesiredLocation));
		CollisionTrace->SetBoolField(TEXT("auto_fallback"), bAutoFallback);
		CollisionTrace->SetNumberField(TEXT("fallback_step_cm"), FallbackStepCm);
		CollisionTrace->SetNumberField(TEXT("candidate_count"), CandidateOffsets.Num());
		CollisionTrace->SetBoolField(TEXT("fallback_used"), bFallbackUsed);
		CollisionTrace->SetNumberField(TEXT("fallback_index"), FallbackIndex);
		CollisionTrace->SetObjectField(TEXT("fallback_offset_cm"), VecToJson(FallbackOffset));
		CollisionTrace->SetObjectField(TEXT("candidate_desired_location"), VecToJson(Best.Desired));
		CollisionTrace->SetStringField(TEXT("trace_channel"), FString::Printf(TEXT("%d"), (int32)TraceChannel));
		CollisionTrace->SetBoolField(TEXT("trace_complex"), bTraceComplex);
		CollisionTrace->SetNumberField(TEXT("safety_offset_cm"), SafetyOffsetCm);
		CollisionTrace->SetBoolField(TEXT("hit"), Best.bHit);
		UeAgentLevelOps::FillHitResultJson(Best.Hit, CollisionTrace);
		CollisionTrace->SetBoolField(TEXT("adjusted"), Best.bAdjusted);
		CollisionTrace->SetObjectField(TEXT("new_location"), VecToJson(NewLocation));
		if (Best.bAdjusted)
		{
			CollisionTrace->SetStringField(TEXT("adjustment_mode"), TEXT("hit_point_minus_trace_dir"));
		}
	}

	LVC->SetViewLocation(NewLocation);
	if (bLookAt)
	{
		const FVector Target = Bounds.GetCenter();
		const FVector Dir = (Target - NewLocation);
		if (!Dir.IsNearlyZero(KINDA_SMALL_NUMBER))
		{
			FRotator LookAtRot = Dir.Rotation();
			LookAtRot.Roll = 0.0f;
			LVC->SetViewRotation(LookAtRot);
		}
	}
	LVC->Invalidate();

	FUeAgentInterfaceLogger::Log(TEXT("CmdViewportFrameActor req=%s actor=%s padding=%.3f"), *Ctx.RequestId, *Id, Padding);
	OutData->SetObjectField(TEXT("actor"), UeAgentLevelOps::MakeActorSummaryJson(Actor));
	OutData->SetNumberField(TEXT("padding"), Padding);
	OutData->SetObjectField(TEXT("bounds"), UeAgentLevelOps::MakeBoundsJson(Bounds));
	OutData->SetObjectField(TEXT("previous_location"), VecToJson(PreviousLocation));
	OutData->SetObjectField(TEXT("desired_location"), VecToJson(DesiredLocation));
	OutData->SetObjectField(TEXT("new_location"), VecToJson(LVC->GetViewLocation()));
	OutData->SetBoolField(TEXT("collision_aware"), bCollisionAware);
	OutData->SetBoolField(TEXT("collision_adjusted"), bCollisionAdjusted);
	OutData->SetBoolField(TEXT("fallback_used"), bFallbackUsed);
	OutData->SetNumberField(TEXT("fallback_index"), FallbackIndex);
	OutData->SetObjectField(TEXT("fallback_offset_cm"), VecToJson(FallbackOffset));
	OutData->SetBoolField(TEXT("look_at"), bLookAt);
	if (CollisionTrace.IsValid())
	{
		OutData->SetObjectField(TEXT("collision_trace"), CollisionTrace);
	}
	OutData->SetObjectField(TEXT("rotation"), RotToJson(LVC->GetViewRotation()));
	OutData->SetNumberField(TEXT("fov"), LVC->ViewFOV);
	OutData->SetBoolField(TEXT("realtime"), LVC->IsRealtime());
	OutData->SetBoolField(TEXT("game_view"), LVC->IsInGameView());
	OutData->SetObjectField(TEXT("viewport"), UeAgentLevelOps::MakeViewportClientJson(LVC, SelectionReason));
	return true;
}

bool FUeAgentHttpServer::CmdViewportFrameSelection(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TSharedPtr<FJsonObject> Focus = MakeShared<FJsonObject>();
	if (Ctx.Params.IsValid())
	{
		Focus->Values = Ctx.Params->Values;
	}
	Focus->SetStringField(TEXT("mode"), TEXT("selection"));
	return UeAgentEditorViewportLegacy::ExecuteFocus(*this, Ctx, UeAgentEditorViewportLegacy::MakeLevelTarget(), Focus, OutData, OutError);
}

bool FUeAgentHttpServer::CmdViewportFrameSelectionCore(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FLevelEditorViewportClient* LVC = UeAgentLevelOps::GetPreferredLevelViewportClient();
	if (!LVC)
	{
		OutError = TEXT("no_level_editor_viewport");
		return false;
	}
	if (!GEditor)
	{
		OutError = TEXT("missing_editor");
		return false;
	}

	const bool bInstant = [&Ctx]()
	{
		bool bValue = true;
		JsonTryGetBool(Ctx.Params, TEXT("instant"), bValue);
		return bValue;
	}();

	USelection* SelectedActors = GEditor->GetSelectedActors();
	if (!SelectedActors || SelectedActors->Num() <= 0)
	{
		OutError = TEXT("selection_empty");
		return false;
	}

	FBox CombinedBox(EForceInit::ForceInit);
	TArray<TSharedPtr<FJsonValue>> FocusedActors;
	for (FSelectionIterator It(*SelectedActors); It; ++It)
	{
		AActor* Actor = Cast<AActor>(*It);
		if (!Actor)
		{
			continue;
		}

		const FBox ActorBox = Actor->GetComponentsBoundingBox(true);
		if (ActorBox.IsValid)
		{
			CombinedBox += ActorBox;
		}
		FocusedActors.Add(MakeShared<FJsonValueObject>(UeAgentLevelOps::MakeActorSummaryJson(Actor)));
	}

	if (!CombinedBox.IsValid || FocusedActors.Num() <= 0)
	{
		OutError = TEXT("selection_has_invalid_bounds");
		return false;
	}

	FUeAgentInterfaceLogger::Log(TEXT("CmdViewportFrameSelection req=%s count=%d instant=%s"), *Ctx.RequestId, FocusedActors.Num(), bInstant ? TEXT("true") : TEXT("false"));
	LVC->FocusViewportOnBox(CombinedBox, bInstant);
	LVC->Invalidate();

	OutData->SetNumberField(TEXT("focused_count"), FocusedActors.Num());
	OutData->SetBoolField(TEXT("instant"), bInstant);
	OutData->SetArrayField(TEXT("actors"), FocusedActors);
	OutData->SetObjectField(TEXT("bounds"), UeAgentLevelOps::MakeBoundsJson(CombinedBox));
	return true;
}

bool FUeAgentHttpServer::CmdViewportFrameActors(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	const TArray<TSharedPtr<FJsonValue>>* ActorIds = nullptr;
	if (!Ctx.Params.IsValid() || !Ctx.Params->TryGetArrayField(TEXT("actor_ids"), ActorIds) || !ActorIds || ActorIds->Num() <= 0)
	{
		OutError = TEXT("missing_actor_ids");
		return false;
	}

	TSharedPtr<FJsonObject> Focus = MakeShared<FJsonObject>();
	if (Ctx.Params.IsValid())
	{
		Focus->Values = Ctx.Params->Values;
	}
	Focus->SetStringField(TEXT("mode"), TEXT("actors"));
	Focus->SetArrayField(TEXT("actor_ids"), *ActorIds);
	return UeAgentEditorViewportLegacy::ExecuteFocus(*this, Ctx, UeAgentEditorViewportLegacy::MakeLevelTarget(), Focus, OutData, OutError);
}

bool FUeAgentHttpServer::CmdViewportFrameActorsCore(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	FLevelEditorViewportClient* LVC = UeAgentLevelOps::GetPreferredLevelViewportClient();
	if (!LVC)
	{
		OutError = TEXT("no_level_editor_viewport");
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* ActorIds = nullptr;
	if (!Ctx.Params.IsValid() || !Ctx.Params->TryGetArrayField(TEXT("actor_ids"), ActorIds) || !ActorIds || ActorIds->Num() <= 0)
	{
		OutError = TEXT("missing_actor_ids");
		return false;
	}

	bool bInstant = true;
	JsonTryGetBool(Ctx.Params, TEXT("instant"), bInstant);

	FBox CombinedBox(EForceInit::ForceInit);
	TArray<TSharedPtr<FJsonValue>> FocusedActors;
	TArray<AActor*> FocusedActorPtrs;
	for (const TSharedPtr<FJsonValue>& Value : *ActorIds)
	{
		const FString ActorId = Value.IsValid() ? Value->AsString() : FString();
		if (ActorId.IsEmpty())
		{
			continue;
		}

		AActor* Actor = FindActorByNameOrLabel(World, ActorId);
		if (!Actor)
		{
			OutError = FString::Printf(TEXT("actor_not_found:%s"), *ActorId);
			return false;
		}

		const FBox ActorBox = Actor->GetComponentsBoundingBox(true);
		if (!ActorBox.IsValid)
		{
			OutError = FString::Printf(TEXT("actor_has_invalid_bounds:%s"), *ActorId);
			return false;
		}

		CombinedBox += ActorBox;
		FocusedActors.Add(MakeShared<FJsonValueObject>(UeAgentLevelOps::MakeActorSummaryJson(Actor)));
		FocusedActorPtrs.Add(Actor);
	}

	if (!CombinedBox.IsValid || FocusedActors.Num() <= 0)
	{
		OutError = TEXT("actor_bounds_invalid");
		return false;
	}

	const FVector PreviousLocation = LVC->GetViewLocation();

	FUeAgentInterfaceLogger::Log(TEXT("CmdViewportFrameActors req=%s count=%d instant=%s"), *Ctx.RequestId, FocusedActors.Num(), bInstant ? TEXT("true") : TEXT("false"));
	LVC->FocusViewportOnBox(CombinedBox, bInstant);

	const FVector DesiredLocation = LVC->GetViewLocation();
	FVector NewLocation = DesiredLocation;

	bool bCollisionAware = false;
	JsonTryGetBool(Ctx.Params, TEXT("collision_aware"), bCollisionAware);

	bool bCollisionAdjusted = false;
	bool bFallbackUsed = false;
	int32 FallbackIndex = 0;
	FVector FallbackOffset = FVector::ZeroVector;
	TSharedPtr<FJsonObject> CollisionTrace;
	if (bCollisionAware)
	{
		double SafetyOffsetCm = 15.0;
		JsonTryGetNumber(Ctx.Params, TEXT("safety_offset_cm"), SafetyOffsetCm);
		SafetyOffsetCm = FMath::Clamp(SafetyOffsetCm, 0.0, 10000.0);

		ECollisionChannel TraceChannel = ECC_Visibility;
		if (!UeAgentLevelOps::ResolveTraceChannel(Ctx.Params, TraceChannel))
		{
			OutError = TEXT("unsupported_trace_channel");
			return false;
		}

		bool bTraceComplex = false;
		JsonTryGetBool(Ctx.Params, TEXT("trace_complex"), bTraceComplex);

		FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(UeAgentViewportFrameActorsCollision), bTraceComplex);
		for (AActor* Actor : FocusedActorPtrs)
		{
			if (Actor)
			{
				QueryParams.AddIgnoredActor(Actor);
			}
		}

		bool bAutoFallback = false;
		JsonTryGetBool(Ctx.Params, TEXT("auto_fallback"), bAutoFallback);

		double FallbackStepCm = 200.0;
		JsonTryGetNumber(Ctx.Params, TEXT("fallback_step_cm"), FallbackStepCm);
		FallbackStepCm = FMath::Clamp(FallbackStepCm, 0.0, 10000.0);

		TArray<FVector> CandidateOffsets;
		CandidateOffsets.Reserve(8);
		CandidateOffsets.Add(FVector::ZeroVector);

		const TArray<TSharedPtr<FJsonValue>>* ExplicitOffsets = nullptr;
		if (Ctx.Params.IsValid() && Ctx.Params->TryGetArrayField(TEXT("fallback_offsets_cm"), ExplicitOffsets) && ExplicitOffsets)
		{
			for (const TSharedPtr<FJsonValue>& Value : *ExplicitOffsets)
			{
				if (!Value.IsValid())
				{
					continue;
				}

				FVector Offset = FVector::ZeroVector;
				if (UeAgentJsonDiagnostics::TryReadVectorValue(Value, Offset) && !Offset.IsNearlyZero())
				{
					CandidateOffsets.Add(Offset);
				}
			}
		}

		const FVector Target = CombinedBox.GetCenter();
		if (bAutoFallback && CandidateOffsets.Num() <= 1 && FallbackStepCm > KINDA_SMALL_NUMBER)
		{
			FVector BaseDir = (DesiredLocation - Target).GetSafeNormal();
			if (BaseDir.IsNearlyZero(KINDA_SMALL_NUMBER))
			{
				BaseDir = (PreviousLocation - Target).GetSafeNormal();
			}
			if (BaseDir.IsNearlyZero(KINDA_SMALL_NUMBER))
			{
				BaseDir = FVector::BackwardVector;
			}

			const FVector WorldUp = FVector::UpVector;
			FVector Right = FVector::CrossProduct(WorldUp, BaseDir).GetSafeNormal();
			if (Right.IsNearlyZero(KINDA_SMALL_NUMBER))
			{
				Right = FVector::CrossProduct(FVector::RightVector, BaseDir).GetSafeNormal();
			}
			FVector Up = FVector::CrossProduct(BaseDir, Right).GetSafeNormal();
			if (Up.IsNearlyZero(KINDA_SMALL_NUMBER))
			{
				Up = WorldUp;
			}

			CandidateOffsets.Add(Right * (float)FallbackStepCm);
			CandidateOffsets.Add(-Right * (float)FallbackStepCm);
			CandidateOffsets.Add(Up * (float)FallbackStepCm);
			CandidateOffsets.Add(-Up * (float)FallbackStepCm);
			CandidateOffsets.Add(BaseDir * (float)FallbackStepCm);
			CandidateOffsets.Add(-BaseDir * (float)FallbackStepCm);
		}

		struct FCollisionCandidate
		{
			int32 Index = 0;
			FVector Offset = FVector::ZeroVector;
			FVector Desired = FVector::ZeroVector;
			FVector Location = FVector::ZeroVector;
			bool bHit = false;
			bool bAdjusted = false;
			FHitResult Hit;
			double Score = 0.0;
		};

		FCollisionCandidate Best;
		Best.Score = -TNumericLimits<double>::Max();

		for (int32 Index = 0; Index < CandidateOffsets.Num(); ++Index)
		{
			const FVector Offset = CandidateOffsets[Index];
			const FVector CandidateDesired = DesiredLocation + Offset;

			FCollisionCandidate Candidate;
			Candidate.Index = Index;
			Candidate.Offset = Offset;
			Candidate.Desired = CandidateDesired;

			const bool bHit = World->LineTraceSingleByChannel(Candidate.Hit, Target, CandidateDesired, TraceChannel, QueryParams);
			Candidate.bHit = bHit;

			if (bHit && Candidate.Hit.bBlockingHit)
			{
				FVector Dir = (CandidateDesired - Target).GetSafeNormal();
				if (Dir.IsNearlyZero(KINDA_SMALL_NUMBER))
				{
					Dir = (DesiredLocation - Target).GetSafeNormal();
				}
				if (Dir.IsNearlyZero(KINDA_SMALL_NUMBER))
				{
					Dir = FVector::BackwardVector;
				}

				Candidate.Location = Candidate.Hit.ImpactPoint - (Dir * (float)SafetyOffsetCm);
				Candidate.bAdjusted = true;
			}
			else
			{
				Candidate.Location = CandidateDesired;
			}

			const double Distance = FVector::Dist(Target, Candidate.Location);
			Candidate.Score = Distance - (Candidate.bAdjusted ? 0.01 : 0.0);
			if (Candidate.Score > Best.Score)
			{
				Best = Candidate;
			}
		}

		NewLocation = Best.Location;
		bCollisionAdjusted = Best.bAdjusted;
		bFallbackUsed = (Best.Index != 0);
		FallbackIndex = Best.Index;
		FallbackOffset = Best.Offset;

		CollisionTrace = MakeShared<FJsonObject>();
		CollisionTrace->SetObjectField(TEXT("target"), VecToJson(Target));
		CollisionTrace->SetObjectField(TEXT("desired_location"), VecToJson(DesiredLocation));
		CollisionTrace->SetBoolField(TEXT("auto_fallback"), bAutoFallback);
		CollisionTrace->SetNumberField(TEXT("fallback_step_cm"), FallbackStepCm);
		CollisionTrace->SetNumberField(TEXT("candidate_count"), CandidateOffsets.Num());
		CollisionTrace->SetBoolField(TEXT("fallback_used"), bFallbackUsed);
		CollisionTrace->SetNumberField(TEXT("fallback_index"), FallbackIndex);
		CollisionTrace->SetObjectField(TEXT("fallback_offset_cm"), VecToJson(FallbackOffset));
		CollisionTrace->SetObjectField(TEXT("candidate_desired_location"), VecToJson(Best.Desired));
		CollisionTrace->SetStringField(TEXT("trace_channel"), FString::Printf(TEXT("%d"), (int32)TraceChannel));
		CollisionTrace->SetBoolField(TEXT("trace_complex"), bTraceComplex);
		CollisionTrace->SetNumberField(TEXT("safety_offset_cm"), SafetyOffsetCm);
		CollisionTrace->SetBoolField(TEXT("hit"), Best.bHit);
		UeAgentLevelOps::FillHitResultJson(Best.Hit, CollisionTrace);
		CollisionTrace->SetBoolField(TEXT("adjusted"), Best.bAdjusted);
		CollisionTrace->SetObjectField(TEXT("new_location"), VecToJson(NewLocation));
		if (Best.bAdjusted)
		{
			CollisionTrace->SetStringField(TEXT("adjustment_mode"), TEXT("hit_point_minus_trace_dir"));
		}
	}

	if (!NewLocation.Equals(DesiredLocation, 0.01))
	{
		LVC->SetViewLocation(NewLocation);
	}

	LVC->Invalidate();

	OutData->SetNumberField(TEXT("focused_count"), FocusedActors.Num());
	OutData->SetBoolField(TEXT("instant"), bInstant);
	OutData->SetArrayField(TEXT("actors"), FocusedActors);
	OutData->SetObjectField(TEXT("bounds"), UeAgentLevelOps::MakeBoundsJson(CombinedBox));
	OutData->SetObjectField(TEXT("previous_location"), VecToJson(PreviousLocation));
	OutData->SetObjectField(TEXT("desired_location"), VecToJson(DesiredLocation));
	OutData->SetObjectField(TEXT("new_location"), VecToJson(LVC->GetViewLocation()));
	OutData->SetBoolField(TEXT("collision_aware"), bCollisionAware);
	OutData->SetBoolField(TEXT("collision_adjusted"), bCollisionAdjusted);
	OutData->SetBoolField(TEXT("fallback_used"), bFallbackUsed);
	OutData->SetNumberField(TEXT("fallback_index"), FallbackIndex);
	OutData->SetObjectField(TEXT("fallback_offset_cm"), VecToJson(FallbackOffset));
	if (CollisionTrace.IsValid())
	{
		OutData->SetObjectField(TEXT("collision_trace"), CollisionTrace);
	}
	return true;
}

bool FUeAgentHttpServer::CmdViewportFrameFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* World = GetEditorWorld(OutError);
	if (!World)
	{
		return false;
	}

	FString SelectionReason;
	FLevelEditorViewportClient* LVC = UeAgentLevelOps::GetPreferredLevelViewportClient(&SelectionReason);
	if (!LVC)
	{
		OutError = TEXT("no_level_editor_viewport");
		return false;
	}

	FString FolderPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderPath) || FolderPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("missing_folder_path");
		return false;
	}

	bool bIncludeChildFolders = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_child_folders"), bIncludeChildFolders);

	double Padding = 1.1;
	JsonTryGetNumber(Ctx.Params, TEXT("padding"), Padding);

	FBox CombinedBounds(EForceInit::ForceInit);
	TArray<AActor*> FolderActors;
	if (!UeAgentLevelOps::GetFolderActorBounds(World, FolderPath, bIncludeChildFolders, CombinedBounds, FolderActors, OutError))
	{
		return false;
	}

	const FVector PreviousLocation = LVC->GetViewLocation();
	const FRotator PreviousRotation = LVC->GetViewRotation();
	FVector DesiredLocation = PreviousLocation;
	FVector NewLocation = PreviousLocation;
	if (!UeAgentLevelOps::ComputeViewportLocationToFrameBounds(LVC, CombinedBounds, PreviousRotation, Padding, NewLocation, OutError))
	{
		return false;
	}

	DesiredLocation = NewLocation;

	bool bCollisionAware = false;
	JsonTryGetBool(Ctx.Params, TEXT("collision_aware"), bCollisionAware);

	bool bLookAt = false;
	JsonTryGetBool(Ctx.Params, TEXT("look_at"), bLookAt);

	bool bCollisionAdjusted = false;
	bool bFallbackUsed = false;
	int32 FallbackIndex = 0;
	FVector FallbackOffset = FVector::ZeroVector;
	TSharedPtr<FJsonObject> CollisionTrace;
	if (bCollisionAware)
	{
		double SafetyOffsetCm = 15.0;
		JsonTryGetNumber(Ctx.Params, TEXT("safety_offset_cm"), SafetyOffsetCm);
		SafetyOffsetCm = FMath::Clamp(SafetyOffsetCm, 0.0, 10000.0);

		ECollisionChannel TraceChannel = ECC_Visibility;
		if (!UeAgentLevelOps::ResolveTraceChannel(Ctx.Params, TraceChannel))
		{
			OutError = TEXT("unsupported_trace_channel");
			return false;
		}

		bool bTraceComplex = false;
		JsonTryGetBool(Ctx.Params, TEXT("trace_complex"), bTraceComplex);

		FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(UeAgentViewportFrameFolderCollision), bTraceComplex);
		for (AActor* Actor : FolderActors)
		{
			if (Actor)
			{
				QueryParams.AddIgnoredActor(Actor);
			}
		}

		bool bAutoFallback = false;
		JsonTryGetBool(Ctx.Params, TEXT("auto_fallback"), bAutoFallback);

		double FallbackStepCm = 200.0;
		JsonTryGetNumber(Ctx.Params, TEXT("fallback_step_cm"), FallbackStepCm);
		FallbackStepCm = FMath::Clamp(FallbackStepCm, 0.0, 10000.0);

		TArray<FVector> CandidateOffsets;
		CandidateOffsets.Reserve(8);
		CandidateOffsets.Add(FVector::ZeroVector);

		const TArray<TSharedPtr<FJsonValue>>* ExplicitOffsets = nullptr;
		if (Ctx.Params.IsValid() && Ctx.Params->TryGetArrayField(TEXT("fallback_offsets_cm"), ExplicitOffsets) && ExplicitOffsets)
		{
			for (const TSharedPtr<FJsonValue>& Value : *ExplicitOffsets)
			{
				if (!Value.IsValid())
				{
					continue;
				}

				FVector Offset = FVector::ZeroVector;
				if (UeAgentJsonDiagnostics::TryReadVectorValue(Value, Offset) && !Offset.IsNearlyZero())
				{
					CandidateOffsets.Add(Offset);
				}
			}
		}

		const FVector Target = CombinedBounds.GetCenter();
		if (bAutoFallback && CandidateOffsets.Num() <= 1 && FallbackStepCm > KINDA_SMALL_NUMBER)
		{
			FVector BaseDir = (DesiredLocation - Target).GetSafeNormal();
			if (BaseDir.IsNearlyZero(KINDA_SMALL_NUMBER))
			{
				BaseDir = (PreviousLocation - Target).GetSafeNormal();
			}
			if (BaseDir.IsNearlyZero(KINDA_SMALL_NUMBER))
			{
				BaseDir = FVector::BackwardVector;
			}

			const FVector WorldUp = FVector::UpVector;
			FVector Right = FVector::CrossProduct(WorldUp, BaseDir).GetSafeNormal();
			if (Right.IsNearlyZero(KINDA_SMALL_NUMBER))
			{
				Right = FVector::CrossProduct(FVector::RightVector, BaseDir).GetSafeNormal();
			}
			FVector Up = FVector::CrossProduct(BaseDir, Right).GetSafeNormal();
			if (Up.IsNearlyZero(KINDA_SMALL_NUMBER))
			{
				Up = WorldUp;
			}

			CandidateOffsets.Add(Right * (float)FallbackStepCm);
			CandidateOffsets.Add(-Right * (float)FallbackStepCm);
			CandidateOffsets.Add(Up * (float)FallbackStepCm);
			CandidateOffsets.Add(-Up * (float)FallbackStepCm);
			CandidateOffsets.Add(BaseDir * (float)FallbackStepCm);
			CandidateOffsets.Add(-BaseDir * (float)FallbackStepCm);
		}

		struct FCollisionCandidate
		{
			int32 Index = 0;
			FVector Offset = FVector::ZeroVector;
			FVector Desired = FVector::ZeroVector;
			FVector Location = FVector::ZeroVector;
			bool bHit = false;
			bool bAdjusted = false;
			FHitResult Hit;
			double Score = 0.0;
		};

		FCollisionCandidate Best;
		Best.Score = -TNumericLimits<double>::Max();

		for (int32 Index = 0; Index < CandidateOffsets.Num(); ++Index)
		{
			const FVector Offset = CandidateOffsets[Index];
			const FVector CandidateDesired = DesiredLocation + Offset;

			FCollisionCandidate Candidate;
			Candidate.Index = Index;
			Candidate.Offset = Offset;
			Candidate.Desired = CandidateDesired;

			const bool bHit = World->LineTraceSingleByChannel(Candidate.Hit, Target, CandidateDesired, TraceChannel, QueryParams);
			Candidate.bHit = bHit;

			if (bHit && Candidate.Hit.bBlockingHit)
			{
				FVector Dir = (CandidateDesired - Target).GetSafeNormal();
				if (Dir.IsNearlyZero(KINDA_SMALL_NUMBER))
				{
					Dir = (DesiredLocation - Target).GetSafeNormal();
				}
				if (Dir.IsNearlyZero(KINDA_SMALL_NUMBER))
				{
					Dir = FVector::BackwardVector;
				}

				Candidate.Location = Candidate.Hit.ImpactPoint - (Dir * (float)SafetyOffsetCm);
				Candidate.bAdjusted = true;
			}
			else
			{
				Candidate.Location = CandidateDesired;
			}

			const double Distance = FVector::Dist(Target, Candidate.Location);
			Candidate.Score = Distance - (Candidate.bAdjusted ? 0.01 : 0.0);
			if (Candidate.Score > Best.Score)
			{
				Best = Candidate;
			}
		}

		NewLocation = Best.Location;
		bCollisionAdjusted = Best.bAdjusted;
		bFallbackUsed = (Best.Index != 0);
		FallbackIndex = Best.Index;
		FallbackOffset = Best.Offset;

		CollisionTrace = MakeShared<FJsonObject>();
		CollisionTrace->SetObjectField(TEXT("target"), VecToJson(Target));
		CollisionTrace->SetObjectField(TEXT("desired_location"), VecToJson(DesiredLocation));
		CollisionTrace->SetBoolField(TEXT("auto_fallback"), bAutoFallback);
		CollisionTrace->SetNumberField(TEXT("fallback_step_cm"), FallbackStepCm);
		CollisionTrace->SetNumberField(TEXT("candidate_count"), CandidateOffsets.Num());
		CollisionTrace->SetBoolField(TEXT("fallback_used"), bFallbackUsed);
		CollisionTrace->SetNumberField(TEXT("fallback_index"), FallbackIndex);
		CollisionTrace->SetObjectField(TEXT("fallback_offset_cm"), VecToJson(FallbackOffset));
		CollisionTrace->SetObjectField(TEXT("candidate_desired_location"), VecToJson(Best.Desired));
		CollisionTrace->SetStringField(TEXT("trace_channel"), FString::Printf(TEXT("%d"), (int32)TraceChannel));
		CollisionTrace->SetBoolField(TEXT("trace_complex"), bTraceComplex);
		CollisionTrace->SetNumberField(TEXT("safety_offset_cm"), SafetyOffsetCm);
		CollisionTrace->SetBoolField(TEXT("hit"), Best.bHit);
		UeAgentLevelOps::FillHitResultJson(Best.Hit, CollisionTrace);
		CollisionTrace->SetBoolField(TEXT("adjusted"), Best.bAdjusted);
		CollisionTrace->SetObjectField(TEXT("new_location"), VecToJson(NewLocation));
		if (Best.bAdjusted)
		{
			CollisionTrace->SetStringField(TEXT("adjustment_mode"), TEXT("hit_point_minus_trace_dir"));
		}
	}

	LVC->SetViewLocation(NewLocation);
	if (bLookAt)
	{
		const FVector Target = CombinedBounds.GetCenter();
		const FVector Dir = (Target - NewLocation);
		if (!Dir.IsNearlyZero(KINDA_SMALL_NUMBER))
		{
			FRotator LookAtRot = Dir.Rotation();
			LookAtRot.Roll = 0.0f;
			LVC->SetViewRotation(LookAtRot);
		}
	}
	LVC->Invalidate();

	TArray<TSharedPtr<FJsonValue>> FocusedActors;
	FocusedActors.Reserve(FolderActors.Num());
	for (AActor* Actor : FolderActors)
	{
		FocusedActors.Add(MakeShared<FJsonValueObject>(UeAgentLevelOps::MakeActorSummaryJson(Actor)));
	}

	FUeAgentInterfaceLogger::Log(
		TEXT("CmdViewportFrameFolder req=%s folder=%s include_child_folders=%s actor_count=%d padding=%.3f"),
		*Ctx.RequestId,
		*FolderPath,
		bIncludeChildFolders ? TEXT("true") : TEXT("false"),
		FolderActors.Num(),
		Padding);

	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetBoolField(TEXT("include_child_folders"), bIncludeChildFolders);
	OutData->SetNumberField(TEXT("focused_count"), FocusedActors.Num());
	OutData->SetNumberField(TEXT("padding"), Padding);
	OutData->SetArrayField(TEXT("actors"), FocusedActors);
	OutData->SetObjectField(TEXT("bounds"), UeAgentLevelOps::MakeBoundsJson(CombinedBounds));
	OutData->SetObjectField(TEXT("previous_location"), VecToJson(PreviousLocation));
	OutData->SetObjectField(TEXT("desired_location"), VecToJson(DesiredLocation));
	OutData->SetObjectField(TEXT("new_location"), VecToJson(LVC->GetViewLocation()));
	OutData->SetBoolField(TEXT("collision_aware"), bCollisionAware);
	OutData->SetBoolField(TEXT("collision_adjusted"), bCollisionAdjusted);
	OutData->SetBoolField(TEXT("fallback_used"), bFallbackUsed);
	OutData->SetNumberField(TEXT("fallback_index"), FallbackIndex);
	OutData->SetObjectField(TEXT("fallback_offset_cm"), VecToJson(FallbackOffset));
	OutData->SetBoolField(TEXT("look_at"), bLookAt);
	if (CollisionTrace.IsValid())
	{
		OutData->SetObjectField(TEXT("collision_trace"), CollisionTrace);
	}
	OutData->SetObjectField(TEXT("rotation"), RotToJson(LVC->GetViewRotation()));
	OutData->SetNumberField(TEXT("fov"), LVC->ViewFOV);
	OutData->SetBoolField(TEXT("realtime"), LVC->IsRealtime());
	OutData->SetBoolField(TEXT("game_view"), LVC->IsInGameView());
	OutData->SetObjectField(TEXT("viewport"), UeAgentLevelOps::MakeViewportClientJson(LVC, SelectionReason));
	return true;
}
