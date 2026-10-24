#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialViewportWarnings.h"

#include "Core/Contract/UeAgentDiagnostic.h"
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

bool UE::UAI::DomainAdapters::WorldSpatial::Private::BuildViewportWarnings(
	UWorld* World,
	const bool bIncludeSuppressed,
	TSharedPtr<FJsonObject>& OutData,
	UE::UAI::Contract::FDiagnostic& OutError)
{
	if (!World)
	{
		OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("no_editor_world"),
			TEXT("An Editor world is required to collect viewport warnings."), TEXT("native_execution"));
		OutError.PropertyPath = TEXT("/world");
		OutError.Source.JsonPath = OutError.PropertyPath;
		OutError.SourceKind = TEXT("native_result");
		OutError.ActualValuePresent = true;
		OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
		OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("available Editor world"));
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
