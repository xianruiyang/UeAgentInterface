// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commands/EditorViewport/UeAgentHttpServer_EditorViewportInternal.h"
#include "Commands/Level/UeAgentHttpServer_LevelOps.h"

#include "Server/UeAgentHttpServer.h"
#include "Utilities/UeAgentJsonDiagnostics.h"

#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "LevelEditorViewport.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "RenderingThread.h"
#include "Slate/SceneViewport.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"

namespace UeAgentEditorViewport
{
	bool TryGetObjectField(const TSharedPtr<FJsonObject>& Obj, const FString& Key, TSharedPtr<FJsonObject>& OutObj)
	{
		const TSharedPtr<FJsonObject>* Found = nullptr;
		if (Obj.IsValid() && Obj->TryGetObjectField(Key, Found) && Found && Found->IsValid())
		{
			OutObj = *Found;
			return true;
		}
		return false;
	}

	bool TryGetStringField(const TSharedPtr<FJsonObject>& Obj, const FString& Key, FString& OutValue)
	{
		return Obj.IsValid() && Obj->TryGetStringField(Key, OutValue);
	}

	bool TryGetNumberField(const TSharedPtr<FJsonObject>& Obj, const FString& Key, double& OutValue)
	{
		return UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { Key }, OutValue);
	}

	bool TryGetBoolField(const TSharedPtr<FJsonObject>& Obj, const FString& Key, bool& OutValue)
	{
		return Obj.IsValid() && Obj->TryGetBoolField(Key, OutValue);
	}

	void CopyJsonFieldIfPresent(const TSharedPtr<FJsonObject>& Source, const FString& Key, const TSharedPtr<FJsonObject>& Dest)
	{
		if (!Source.IsValid() || !Dest.IsValid())
		{
			return;
		}
		const TSharedPtr<FJsonValue>* Value = Source->Values.Find(Key);
		if (Value && Value->IsValid())
		{
			Dest->Values.Add(Key, *Value);
		}
	}

	static bool TryReadVectorObject(const TSharedPtr<FJsonObject>& Obj, FVector& OutValue)
	{
		return UeAgentJsonDiagnostics::TryReadVectorObject(Obj, OutValue);
	}

	static bool TryReadRotatorObject(const TSharedPtr<FJsonObject>& Obj, FRotator& OutValue)
	{
		return UeAgentJsonDiagnostics::TryReadRotatorObject(Obj, OutValue);
	}

	bool TryReadVectorField(const TSharedPtr<FJsonObject>& Obj, const FString& Key, FVector& OutValue)
	{
		TSharedPtr<FJsonObject> VectorObj;
		if (TryGetObjectField(Obj, Key, VectorObj))
		{
			return TryReadVectorObject(VectorObj, OutValue);
		}
		const TSharedPtr<FJsonValue>* FieldValue = UeAgentJsonDiagnostics::FindFieldValue(Obj, Key);
		return FieldValue && UeAgentJsonDiagnostics::TryReadVectorValue(*FieldValue, OutValue);
	}

	bool TryReadRotatorField(const TSharedPtr<FJsonObject>& Obj, const FString& Key, FRotator& OutValue)
	{
		TSharedPtr<FJsonObject> RotatorObj;
		if (TryGetObjectField(Obj, Key, RotatorObj))
		{
			return TryReadRotatorObject(RotatorObj, OutValue);
		}
		const TSharedPtr<FJsonValue>* FieldValue = UeAgentJsonDiagnostics::FindFieldValue(Obj, Key);
		return FieldValue && UeAgentJsonDiagnostics::TryReadRotatorValue(*FieldValue, OutValue);
	}

	bool HasUserFields(const TSharedPtr<FJsonObject>& Obj)
	{
		return Obj.IsValid() && Obj->Values.Num() > 0;
	}

	bool FindWidgetByTypeRecursive(const TSharedRef<SWidget>& RootWidget, const FName& TypeName, TSharedPtr<SWidget>& OutWidget)
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

	FString NormalizeToken(FString Value)
	{
		Value.TrimStartAndEndInline();
		Value.ToLowerInline();
		Value.ReplaceInline(TEXT("-"), TEXT("_"));
		return Value;
	}

	FString ViewModeToString(const EViewModeIndex ViewMode)
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

	bool TryParseViewMode(const FString& InValue, EViewModeIndex& OutViewMode)
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

	FViewportTarget ParseTarget(const TSharedPtr<FJsonObject>& Params)
	{
		FViewportTarget Target;
		TSharedPtr<FJsonObject> TargetObj;
		const TSharedPtr<FJsonObject>& Source = TryGetObjectField(Params, TEXT("target"), TargetObj) ? TargetObj : Params;
		TryGetStringField(Source, TEXT("scope"), Target.Scope);
		TryGetStringField(Source, TEXT("asset_path"), Target.AssetPath);
		TryGetStringField(Source, TEXT("asset_type"), Target.AssetType);
		TryGetStringField(Source, TEXT("viewport_role"), Target.ViewportRole);
		TryGetStringField(Source, TEXT("viewport"), Target.Viewport);
		TryGetStringField(Source, TEXT("viewport_id"), Target.ViewportId);
		TryGetBoolField(Source, TEXT("open_editor_if_needed"), Target.bOpenEditorIfNeeded);
		if (Target.Scope.IsEmpty())
		{
			Target.Scope = (!Target.AssetPath.IsEmpty() || !Target.AssetType.IsEmpty()) ? TEXT("asset_editor") : TEXT("level");
		}
		Target.Scope = NormalizeToken(Target.Scope);
		Target.AssetType = NormalizeToken(Target.AssetType);
		Target.ViewportRole = NormalizeToken(Target.ViewportRole.IsEmpty() ? TEXT("primary") : Target.ViewportRole);
		Target.Viewport = NormalizeToken(Target.Viewport);
		if (Target.Viewport.IsEmpty() && Target.Scope != TEXT("level"))
		{
			Target.Viewport = TEXT("active");
		}
		return Target;
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

	TSharedPtr<FJsonObject> MakeLevelVisualOptionsJson(FLevelEditorViewportClient* Client)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (!Client)
		{
			return Obj;
		}
		const TSharedPtr<FJsonObject> ViewModeState = UeAgentLevelOps::MakeCurrentViewportViewModeJson(Client);
		FString StableViewMode;
		if (!ViewModeState->TryGetStringField(TEXT("view_mode_name"), StableViewMode))
		{
			StableViewMode = UeAgentLevelOps::ViewModeIndexToStableName(Client->GetViewMode());
		}
		Obj->SetStringField(TEXT("view_mode"), StableViewMode);
		Obj->SetNumberField(TEXT("view_mode_value"), static_cast<int32>(Client->GetViewMode()));
		FString VisualizationFamily;
		if (ViewModeState->TryGetStringField(TEXT("visualization_family"), VisualizationFamily))
		{
			Obj->SetStringField(TEXT("visualization_family"), VisualizationFamily);
		}
		FString VisualizationMode;
		if (ViewModeState->TryGetStringField(TEXT("visualization_mode"), VisualizationMode))
		{
			Obj->SetStringField(TEXT("visualization_mode"), VisualizationMode);
		}
		Obj->SetObjectField(TEXT("view_mode_state"), ViewModeState);
		TMap<FString, bool> Overlays;
		Overlays.Add(TEXT("grid"), Client->EngineShowFlags.Grid != 0);
		Overlays.Add(TEXT("bounds"), Client->EngineShowFlags.Bounds != 0);
		Overlays.Add(TEXT("pivot"), Client->EngineShowFlags.Pivot != 0);
		Overlays.Add(TEXT("selection"), Client->EngineShowFlags.Selection != 0);
		Overlays.Add(TEXT("mode_widgets"), Client->EngineShowFlags.ModeWidgets != 0);
		Overlays.Add(TEXT("collision"), Client->EngineShowFlags.Collision != 0);
		Obj->SetObjectField(TEXT("overlays"), MakeBoolOptionsJson(Overlays));
		Obj->SetBoolField(TEXT("realtime"), Client->IsRealtime());
		Obj->SetBoolField(TEXT("game_view"), Client->IsInGameView());
		return Obj;
	}

	bool TryReadBoolOption(const TSharedPtr<FJsonObject>& Options, const FString& Key, bool& OutValue)
	{
		if (TryGetBoolField(Options, Key, OutValue))
		{
			return true;
		}
		TSharedPtr<FJsonObject> Overlays;
		return TryGetObjectField(Options, TEXT("overlays"), Overlays) && TryGetBoolField(Overlays, Key, OutValue);
	}




















	bool ParseCameraRequest(const TSharedPtr<FJsonObject>& Params, const FViewportCamera& CurrentCamera, FViewportCamera& OutCamera, FString& OutError)
	{
		TSharedPtr<FJsonObject> CameraObj;
		const TSharedPtr<FJsonObject>& Source = TryGetObjectField(Params, TEXT("camera"), CameraObj) ? CameraObj : Params;
		if (!Source.IsValid())
		{
			OutError = TEXT("missing_camera");
			return false;
		}

		OutCamera = CurrentCamera;
		OutCamera.bValid = true;

		TSharedPtr<FJsonObject> OrbitObj;
		if (TryGetObjectField(Source, TEXT("orbit"), OrbitObj))
		{
			FVector Target = FVector::ZeroVector;
			TryReadVectorField(OrbitObj, TEXT("target"), Target);

			double Yaw = CurrentCamera.Rotation.Yaw;
			double Pitch = CurrentCamera.Rotation.Pitch;
			double Roll = 0.0;
			double Distance = 500.0;
			double Fov = CurrentCamera.Fov;
			TryGetNumberField(OrbitObj, TEXT("yaw"), Yaw);
			TryGetNumberField(OrbitObj, TEXT("pitch"), Pitch);
			TryGetNumberField(OrbitObj, TEXT("roll"), Roll);
			TryGetNumberField(OrbitObj, TEXT("distance"), Distance);
			TryGetNumberField(OrbitObj, TEXT("fov"), Fov);

			const FRotator Rotation(Pitch, Yaw, Roll);
			OutCamera.Rotation = Rotation;
			OutCamera.Location = Target - Rotation.Vector() * static_cast<float>(FMath::Max(Distance, 1.0));
			OutCamera.Fov = static_cast<float>(FMath::Clamp(Fov, 5.0, 170.0));
			return true;
		}

		bool bAnyField = false;
		bAnyField |= TryReadVectorField(Source, TEXT("location"), OutCamera.Location);
		bAnyField |= TryReadRotatorField(Source, TEXT("rotation"), OutCamera.Rotation);
		double Fov = OutCamera.Fov;
		if (TryGetNumberField(Source, TEXT("fov"), Fov))
		{
			OutCamera.Fov = static_cast<float>(FMath::Clamp(Fov, 5.0, 170.0));
			bAnyField = true;
		}
		FString Projection;
		if (TryGetStringField(Source, TEXT("projection"), Projection))
		{
			OutCamera.Projection = NormalizeToken(Projection);
			bAnyField = true;
		}
		if (!bAnyField)
		{
			OutError = TEXT("missing_camera_fields");
			return false;
		}
		return true;
	}
}
