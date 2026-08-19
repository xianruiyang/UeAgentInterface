// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commands/EditorViewport/UeAgentEditorViewportBlueprint.h"

#include "BlueprintEditor.h"
#include "BlueprintEditorModes.h"
#include "BlueprintEditorTabs.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Editor/EditorPerProjectUserSettings.h"
#include "EditorViewportClient.h"
#include "EditorViewportCommands.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "Framework/Commands/InputBindingManager.h"
#include "Framework/Commands/UICommandList.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PreviewScene.h"
#include "SEditorViewport.h"
#include "Server/UeAgentHttpServer.h"
#include "ShowFlags.h"
#include "Slate/SceneViewport.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UeAgentInterfaceSettings.h"
#include "Utilities/UeAgentJsonDiagnostics.h"
#include "Widgets/Docking/SDockTab.h"
#include "UObject/UObjectIterator.h"

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
		return UeAgentJsonDiagnostics::TryReadVectorObject(Obj, OutValue);
	}

	static bool TryReadRotatorObject(const TSharedPtr<FJsonObject>& Obj, FRotator& OutValue)
	{
		return UeAgentJsonDiagnostics::TryReadRotatorObject(Obj, OutValue);
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

		static bool TryReadRotatorField(const TSharedPtr<FJsonObject>& Obj, const FString& Key, FRotator& OutValue)
		{
			TSharedPtr<FJsonObject> RotatorObj;
			if (TryGetObjectField(Obj, Key, RotatorObj))
			{
				return TryReadRotatorObject(RotatorObj, OutValue);
			}
			const TSharedPtr<FJsonValue>* FieldValue = UeAgentJsonDiagnostics::FindFieldValue(Obj, Key);
			return FieldValue && UeAgentJsonDiagnostics::TryReadRotatorValue(*FieldValue, OutValue);
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

		static TSharedPtr<FJsonObject> MakeBoolOptionsJson(const TMap<FString, bool>& Values)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			for (const TPair<FString, bool>& Pair : Values)
			{
				Obj->SetBoolField(Pair.Key, Pair.Value);
			}
			return Obj;
		}

		static TSharedPtr<FUICommandInfo> FindBlueprintEditorCommand(const FName CommandName)
		{
			return FInputBindingManager::Get().FindCommandInContext(FName(TEXT("BlueprintEditor")), CommandName);
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

		static bool TryExecuteViewportCommand(const TSharedPtr<SEditorViewport>& ViewportWidget, const TSharedPtr<FUICommandInfo>& Command)
		{
			const TSharedPtr<FUICommandList> CommandList = ViewportWidget.IsValid() ? ViewportWidget->GetCommandList() : nullptr;
			return CommandList.IsValid() && Command.IsValid() && CommandList->TryExecuteAction(Command.ToSharedRef());
		}



		;





		static bool ParseCameraRequest(const TSharedPtr<FJsonObject>& Params, const FViewportCamera& CurrentCamera, FViewportCamera& OutCamera, FString& OutError)
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

		static UBlueprint* LoadBlueprintAsset(FString AssetPath)
		{
			AssetPath.TrimStartAndEndInline();
			if (AssetPath.IsEmpty())
			{
				return nullptr;
			}
			if (UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath))
			{
				return Blueprint;
			}
			if (!AssetPath.Contains(TEXT(".")))
			{
				const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *AssetPath, *FPackageName::GetShortName(AssetPath));
				return LoadObject<UBlueprint>(nullptr, *ObjectPath);
			}
			return nullptr;
		}

		static bool IsBlueprintSCSAssetType(const FString& AssetType)
		{
			const FString Type = NormalizeToken(AssetType);
			return Type.IsEmpty() || Type == TEXT("blueprint") || Type == TEXT("actor_blueprint") || Type == TEXT("blueprint_scs");
		}

		static FBox MakeActorBoundsBox(const AActor* Actor)
		{
			if (!Actor)
			{
				return FBox(ForceInit);
			}
			FBox Box = Actor->GetComponentsBoundingBox(true);
			if (!Box.IsValid)
			{
				if (const USceneComponent* Root = Actor->GetRootComponent())
				{
					Box = FBox::BuildAABB(Root->GetComponentLocation(), FVector(25.0f));
				}
			}
			return Box;
		}

		static FBox MakeComponentBoundsBox(const UActorComponent* Component)
		{
			if (const UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Component))
			{
				const FBoxSphereBounds Bounds = Primitive->Bounds;
				return FBox(Bounds.Origin - Bounds.BoxExtent, Bounds.Origin + Bounds.BoxExtent);
			}
			if (const USceneComponent* SceneComponent = Cast<USceneComponent>(Component))
			{
				return FBox::BuildAABB(SceneComponent->GetComponentLocation(), FVector(25.0f));
			}
			return FBox(ForceInit);
		}

		static bool ComponentNameMatches(const UActorComponent* Component, const FString& InName)
		{
			if (!Component)
			{
				return false;
			}
			FString Name = InName;
			Name.TrimStartAndEndInline();
			return Component->GetName().Equals(Name, ESearchCase::IgnoreCase)
				|| Component->GetFName().ToString().Equals(Name, ESearchCase::IgnoreCase)
				|| Component->GetReadableName().Equals(Name, ESearchCase::IgnoreCase);
		}

		static UActorComponent* FindComponentByName(AActor* Actor, const FString& Name)
		{
			if (!Actor || Name.TrimStartAndEnd().IsEmpty())
			{
				return nullptr;
			}
			for (UActorComponent* Component : Actor->GetComponents())
			{
				if (ComponentNameMatches(Component, Name))
				{
					return Component;
				}
			}
			return nullptr;
		}

		static UStaticMeshComponent* FindBlueprintEditorFloorComponent(FPreviewScene* PreviewScene)
		{
			UWorld* PreviewWorld = PreviewScene ? PreviewScene->GetWorld() : nullptr;
			if (!PreviewWorld)
			{
				return nullptr;
			}
			for (TObjectIterator<UStaticMeshComponent> It; It; ++It)
			{
				UStaticMeshComponent* Component = *It;
				if (Component && Component->GetWorld() == PreviewWorld && Component->GetFName() == FName(TEXT("EditorFloorComp")))
				{
					return Component;
				}
			}
			return nullptr;
		}

		static TSharedPtr<FJsonObject> MakeBlueprintVisualOptionsJson(FEditorViewportClient& Client, FPreviewScene* PreviewScene, const TSharedPtr<SEditorViewport>& ViewportWidget)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("view_mode"), ViewModeToString(Client.GetViewMode()));
			Obj->SetNumberField(TEXT("view_mode_value"), static_cast<int32>(Client.GetViewMode()));
			TMap<FString, bool> Overlays;
			bool bGrid = GetDefault<UEditorPerProjectUserSettings>()->bSCSEditorShowGrid;
			TryReadCommandBool(ViewportWidget.IsValid() ? ViewportWidget->GetCommandList() : nullptr, FindBlueprintEditorCommand(FName(TEXT("ShowGrid"))), bGrid);
			Overlays.Add(TEXT("grid"), bGrid);
			Overlays.Add(TEXT("bounds"), Client.EngineShowFlags.Bounds != 0);
			if (UStaticMeshComponent* Floor = FindBlueprintEditorFloorComponent(PreviewScene))
			{
				Overlays.Add(TEXT("floor"), Floor->IsVisible());
			}
			else
			{
				Overlays.Add(TEXT("floor"), GetDefault<UEditorPerProjectUserSettings>()->bSCSEditorShowFloor);
			}
			Obj->SetObjectField(TEXT("overlays"), MakeBoolOptionsJson(Overlays));
			Obj->SetBoolField(TEXT("realtime"), Client.IsRealtime());
			return Obj;
		}

		static TSharedPtr<FJsonObject> MakeBlueprintPreviewStateJson(UBlueprint& Blueprint, FBlueprintEditor& BlueprintEditor)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("context"), TEXT("blueprint_scs"));
			Obj->SetStringField(TEXT("blueprint_path"), Blueprint.GetPathName());
			if (Blueprint.GeneratedClass)
			{
				Obj->SetStringField(TEXT("generated_class"), Blueprint.GeneratedClass->GetPathName());
			}
			if (AActor* PreviewActor = BlueprintEditor.GetPreviewActor())
			{
				Obj->SetStringField(TEXT("preview_actor_name"), PreviewActor->GetName());
				Obj->SetNumberField(TEXT("component_count"), PreviewActor->GetComponents().Num());
				TArray<TSharedPtr<FJsonValue>> ComponentNames;
				for (UActorComponent* Component : PreviewActor->GetComponents())
				{
					if (Component)
					{
						ComponentNames.Add(MakeShared<FJsonValueString>(Component->GetName()));
					}
				}
				Obj->SetArrayField(TEXT("components"), ComponentNames);
			}
			return Obj;
		}

		class FBlueprintSCSViewportAdapter final : public IViewportAdapter
		{
		public:
			virtual FString GetName() const override
			{
				return TEXT("blueprint_scs");
			}

			virtual bool CanResolve(const FResolveRequest& Request) const override
			{
				return Request.Target.Scope == TEXT("asset_editor") && IsBlueprintSCSAssetType(Request.Target.AssetType);
			}

			virtual bool Resolve(const FResolveRequest& Request, FResolvedViewport& OutViewport, FString& OutError) const override
			{
				if (Request.Target.AssetPath.IsEmpty())
				{
					OutError = TEXT("missing_asset_path");
					return false;
				}
				UBlueprint* Blueprint = LoadBlueprintAsset(Request.Target.AssetPath);
				if (!Blueprint)
				{
					OutError = TEXT("blueprint_asset_not_found");
					return false;
				}
				if (!FBlueprintEditorUtils::IsActorBased(Blueprint))
				{
					OutError = TEXT("blueprint_scs_requires_actor_blueprint");
					return false;
				}
				FBlueprintEditor* BlueprintEditor = GetBlueprintEditor(Blueprint, Request.Target.bOpenEditorIfNeeded, OutError);
				if (!BlueprintEditor)
				{
					return false;
				}
				TSharedPtr<SEditorViewport> ViewportWidget = ResolveSCSViewportWidget(*BlueprintEditor, OutError);
				if (!ViewportWidget.IsValid())
				{
					return false;
				}
				TSharedPtr<FEditorViewportClient> Client = ViewportWidget->GetViewportClient();
				if (!Client.IsValid())
				{
					OutError = TEXT("blueprint_scs_viewport_client_not_found");
					return false;
				}

				OutViewport.Target = Request.Target;
				OutViewport.Target.Scope = TEXT("asset_editor");
				OutViewport.Target.AssetPath = Blueprint->GetPathName();
				OutViewport.Target.AssetType = TEXT("blueprint");
				OutViewport.Target.ViewportRole = TEXT("components");
				OutViewport.Target.ViewportId = FString::Printf(TEXT("asset:%s#blueprint_scs"), *Blueprint->GetPathName());
				OutViewport.AdapterName = GetName();
				OutViewport.ClientClass = TEXT("FSCSEditorViewportClient");
				OutViewport.ViewportClient = Client.Get();
				OutViewport.Asset = Blueprint;
				return true;
			}

			virtual bool ReadState(const FResolvedViewport& Viewport, FViewportState& OutState, FString& OutError) const override
			{
				UBlueprint* Blueprint = Cast<UBlueprint>(Viewport.Asset);
				FBlueprintEditor* BlueprintEditor = GetBlueprintEditorFromResolved(Viewport, OutError);
				if (!Blueprint || !BlueprintEditor || !Viewport.ViewportClient)
				{
					OutError = OutError.IsEmpty() ? TEXT("invalid_blueprint_scs_viewport") : OutError;
					return false;
				}
				OutState.Target = Viewport.Target;
				OutState.ClientClass = TEXT("FSCSEditorViewportClient");
				OutState.Adapter = GetName();
				OutState.Camera.Location = Viewport.ViewportClient->GetViewLocation();
				OutState.Camera.Rotation = Viewport.ViewportClient->GetViewRotation();
				OutState.Camera.Fov = Viewport.ViewportClient->ViewFOV;
				OutState.Camera.Space = TEXT("blueprint_preview_world");
				OutState.Camera.Projection = Viewport.ViewportClient->ViewportType == LVT_Perspective ? TEXT("perspective") : TEXT("orthographic");
				OutState.Camera.bValid = true;
				OutState.Capabilities.bCamera = true;
				OutState.Capabilities.bVisualOptions = true;
				OutState.Capabilities.bPreviewState = true;

				OutState.Capabilities.FocusModes = { TEXT("actor"), TEXT("actor_bounds"), TEXT("bounds"), TEXT("component"), TEXT("selection"), TEXT("custom_bounds"), TEXT("custom_point") };

				OutState.Capabilities.VisualOptionKeys = { TEXT("view_mode"), TEXT("grid"), TEXT("floor"), TEXT("bounds"), TEXT("realtime") };
				FString WidgetError;
				TSharedPtr<SEditorViewport> ViewportWidget = ResolveSCSViewportWidget(*BlueprintEditor, WidgetError);
				OutState.VisualOptions = MakeBlueprintVisualOptionsJson(*Viewport.ViewportClient, BlueprintEditor->GetPreviewScene(), ViewportWidget);
				OutState.PreviewState = MakeBlueprintPreviewStateJson(*Blueprint, *BlueprintEditor);
				return true;
			}

			virtual bool WriteCamera(const FResolvedViewport& Viewport, const FViewportCamera& Camera, FString& OutError) const override
			{
				if (!Viewport.ViewportClient)
				{
					OutError = TEXT("invalid_blueprint_scs_viewport");
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
				FBlueprintEditor* BlueprintEditor = GetBlueprintEditorFromResolved(Viewport, OutError);
				if (!BlueprintEditor || !Viewport.ViewportClient)
				{
					OutError = OutError.IsEmpty() ? TEXT("invalid_blueprint_scs_viewport") : OutError;
					return false;
				}
				AActor* PreviewActor = BlueprintEditor->GetPreviewActor();
				if (!PreviewActor)
				{
					BlueprintEditor->UpdateSubobjectPreview(true);
					PreviewActor = BlueprintEditor->GetPreviewActor();
				}
				if (!PreviewActor)
				{
					OutError = TEXT("blueprint_preview_actor_not_found");
					return false;
				}

				FString Mode = TEXT("actor_bounds");
				TryGetStringField(FocusObj, TEXT("mode"), Mode);
				Mode = NormalizeToken(Mode);
				FBox FocusBox(ForceInit);
				FVector FocusPoint = FVector::ZeroVector;
				bool bUsePoint = false;
				bool bSelectionFocusDelegated = false;
				if (Mode == TEXT("actor") || Mode == TEXT("actor_bounds") || Mode == TEXT("bounds"))
				{
					FocusBox = MakeActorBoundsBox(PreviewActor);
				}
				else if (Mode == TEXT("selection"))
				{
					if (TSharedPtr<SEditorViewport> ViewportWidget = ResolveSCSViewportWidget(*BlueprintEditor, OutError))
					{
						bSelectionFocusDelegated = TryExecuteViewportCommand(ViewportWidget, FEditorViewportCommands::Get().FocusViewportToSelection);
					}
					FocusBox = MakeActorBoundsBox(PreviewActor);
				}
				else if (Mode == TEXT("component"))
				{
					FString ComponentName;
					if (!TryGetStringField(FocusObj, TEXT("component"), ComponentName)
						&& !TryGetStringField(FocusObj, TEXT("component_name"), ComponentName)
						&& !TryGetStringField(FocusObj, TEXT("name"), ComponentName))
					{
						OutError = TEXT("missing_component_name");
						return false;
					}
					UActorComponent* Component = FindComponentByName(PreviewActor, ComponentName);
					if (!Component)
					{
						OutError = TEXT("blueprint_component_not_found");
						return false;
					}
					FocusBox = MakeComponentBoundsBox(Component);
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
					OutError = TEXT("unsupported_blueprint_scs_focus_mode");
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
				if (!FocusBox.IsValid)
				{
					OutError = TEXT("invalid_focus_bounds");
					return false;
				}
				if (!bSelectionFocusDelegated)
				{
					Viewport.ViewportClient->FocusViewportOnBox(FocusBox, bInstant);
					Viewport.ViewportClient->SetLookAtLocation(FocusBox.GetCenter(), false);
				}
				Viewport.ViewportClient->Invalidate();

				OutData = MakeShared<FJsonObject>();
				OutData->SetStringField(TEXT("mode"), Mode);
				OutData->SetStringField(TEXT("space"), TEXT("blueprint_preview_world"));
				OutData->SetStringField(TEXT("bounds_space"), TEXT("blueprint_preview_world"));
				OutData->SetBoolField(TEXT("selection_focus_delegated"), bSelectionFocusDelegated);
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
				FBlueprintEditor* BlueprintEditor = GetBlueprintEditorFromResolved(Viewport, OutError);
				if (!BlueprintEditor || !Viewport.ViewportClient)
				{
					OutError = OutError.IsEmpty() ? TEXT("invalid_blueprint_scs_viewport") : OutError;
					return false;
				}
				TArray<FString> Unsupported;
				FString ViewModeString;
				if (TryGetStringField(Options, TEXT("view_mode"), ViewModeString))
				{
					EViewModeIndex ViewMode = VMI_Lit;
					if (!TryParseViewMode(ViewModeString, ViewMode))
					{
						OutError = TEXT("unsupported_view_mode");
						return false;
					}
					Viewport.ViewportClient->SetViewMode(ViewMode);
				}
				FString WidgetError;
				TSharedPtr<SEditorViewport> ViewportWidget = ResolveSCSViewportWidget(*BlueprintEditor, WidgetError);
				const TSharedPtr<FUICommandList> CommandList = ViewportWidget.IsValid() ? ViewportWidget->GetCommandList() : nullptr;
				bool bValue = false;
				if (TryReadBoolOption(Options, TEXT("grid"), bValue))
				{
					TrySetCommandBool(CommandList, FindBlueprintEditorCommand(FName(TEXT("ShowGrid"))), bValue);
					GetMutableDefault<UEditorPerProjectUserSettings>()->bSCSEditorShowGrid = bValue;
				}
				if (TryReadBoolOption(Options, TEXT("bounds"), bValue))
				{
					Viewport.ViewportClient->EngineShowFlags.SetBounds(bValue);
				}
				if (TryReadBoolOption(Options, TEXT("floor"), bValue))
				{
					if (!TrySetCommandBool(CommandList, FindBlueprintEditorCommand(FName(TEXT("ShowFloor"))), bValue))
					{
						SetFloorVisible(BlueprintEditor->GetPreviewScene(), bValue);
					}
				}
				if (TryGetBoolField(Options, TEXT("realtime"), bValue))
				{
					Viewport.ViewportClient->SetRealtime(bValue);
				}
				UEditorPerProjectUserSettings* Settings = GetMutableDefault<UEditorPerProjectUserSettings>();
				if (Settings)
				{
					Settings->PostEditChange();
				}
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Options->Values)
				{
					const FString Key = NormalizeToken(Pair.Key);
					if (Key != TEXT("overlays") && Key != TEXT("view_mode") && Key != TEXT("grid") && Key != TEXT("floor") && Key != TEXT("bounds") && Key != TEXT("realtime"))
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
						if (Key != TEXT("grid") && Key != TEXT("floor") && Key != TEXT("bounds"))
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
				OutData = MakeBlueprintVisualOptionsJson(*Viewport.ViewportClient, BlueprintEditor->GetPreviewScene(), ViewportWidget);
				if (Unsupported.Num() > 0)
				{
					OutData->SetArrayField(TEXT("unsupported_options"), MakeStringArrayJson(Unsupported));
				}
				return true;
			}

			virtual bool ApplyPreviewState(const FResolvedViewport& Viewport, const TSharedPtr<FJsonObject>& PreviewState, TSharedPtr<FJsonObject>& OutData, FString& OutError) const override
			{
				if (!HasUserFields(PreviewState))
				{
					OutData = MakeShared<FJsonObject>();
					OutData->SetBoolField(TEXT("applied"), false);
					OutData->SetStringField(TEXT("reason"), TEXT("empty_preview_state"));
					return true;
				}
				OutError = TEXT("editor_viewport_preview_state_not_supported");
				return false;
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
				return ApplyVisualOptions(Viewport, Snapshot.State.VisualOptions, Applied, OutError);
			}

		private:
			static FBlueprintEditor* GetBlueprintEditor(UBlueprint* Blueprint, const bool bOpenIfNeeded, FString& OutError)
			{
				if (!Blueprint || !GEditor)
				{
					OutError = TEXT("invalid_blueprint_asset");
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
					AssetEditorSubsystem->OpenEditorForAsset(Blueprint);
					if (FSlateApplication::IsInitialized())
					{
						FSlateApplication::Get().Tick();
					}
				}
				IAssetEditorInstance* EditorInstance = AssetEditorSubsystem->FindEditorForAsset(Blueprint, false);
				if (!EditorInstance || EditorInstance->GetEditorName() != FName(TEXT("BlueprintEditor")))
				{
					OutError = TEXT("blueprint_editor_not_opened");
					return nullptr;
				}
				FBlueprintEditor* BlueprintEditor = static_cast<FBlueprintEditor*>(EditorInstance);
				if (!BlueprintEditor || !BlueprintEditor->CanAccessComponentsMode())
				{
					OutError = TEXT("blueprint_components_mode_unavailable");
					return nullptr;
				}
				BlueprintEditor->SetCurrentMode(FBlueprintEditorApplicationModes::BlueprintComponentsMode);
				BlueprintEditor->UpdateSubobjectPreview(true);
				if (FSlateApplication::IsInitialized())
				{
					FSlateApplication::Get().Tick();
				}
				return BlueprintEditor;
			}

			static FBlueprintEditor* GetBlueprintEditorFromResolved(const FResolvedViewport& Viewport, FString& OutError)
			{
				UBlueprint* Blueprint = Cast<UBlueprint>(Viewport.Asset);
				return GetBlueprintEditor(Blueprint, false, OutError);
			}

			static TSharedPtr<SEditorViewport> ResolveSCSViewportWidget(FBlueprintEditor& BlueprintEditor, FString& OutError)
			{
				const TSharedPtr<FTabManager> TabManager = BlueprintEditor.GetAssociatedTabManager();
				if (!TabManager.IsValid())
				{
					OutError = TEXT("blueprint_tab_manager_not_found");
					return nullptr;
				}
				auto ActivateViewportTab = [](const TSharedPtr<SDockTab>& ViewportTab)
				{
					if (ViewportTab.IsValid())
					{
						ViewportTab->ActivateInParent(ETabActivationCause::SetDirectly);
						ViewportTab->SlatePrepass();
						if (FSlateApplication::IsInitialized())
						{
							FSlateApplication::Get().Tick();
						}
					}
				};
				auto ResolveFromTab = [](const TSharedPtr<SDockTab>& ViewportTab) -> TSharedPtr<SEditorViewport>
				{
					if (!ViewportTab.IsValid())
					{
						return nullptr;
					}
					TSharedPtr<SWidget> FoundWidget;
					if (!FindWidgetByTypeRecursive(ViewportTab->GetContent(), FName(TEXT("SSCSEditorViewport")), FoundWidget))
					{
						return nullptr;
					}
					const TSharedPtr<SEditorViewport> ViewportWidget = StaticCastSharedPtr<SEditorViewport>(FoundWidget);
					return ViewportWidget.IsValid() && ViewportWidget->GetViewportClient().IsValid() ? ViewportWidget : nullptr;
				};

				TSharedPtr<SDockTab> ViewportTab = TabManager->FindExistingLiveTab(FTabId(FBlueprintEditorTabs::SCSViewportID));
				ActivateViewportTab(ViewportTab);
				TSharedPtr<SEditorViewport> ViewportWidget = ResolveFromTab(ViewportTab);
				if (ViewportWidget.IsValid())
				{
					return ViewportWidget;
				}
				ViewportTab = TabManager->TryInvokeTab(FTabId(FBlueprintEditorTabs::SCSViewportID), false);
				ActivateViewportTab(ViewportTab);
				ViewportWidget = ResolveFromTab(ViewportTab);
				if (!ViewportWidget.IsValid())
				{
					OutError = TEXT("blueprint_scs_viewport_widget_not_found");
				}
				return ViewportWidget;
			}

			static void SetFloorVisible(FPreviewScene* PreviewScene, const bool bVisible)
			{
				if (UStaticMeshComponent* Floor = FindBlueprintEditorFloorComponent(PreviewScene))
				{
					Floor->SetVisibility(bVisible);
					Floor->SetCollisionEnabled(bVisible ? ECollisionEnabled::QueryAndPhysics : ECollisionEnabled::NoCollision);
				}
				UEditorPerProjectUserSettings* Settings = GetMutableDefault<UEditorPerProjectUserSettings>();
				Settings->bSCSEditorShowFloor = bVisible;
			}
		};
	}

	TSharedRef<IViewportAdapter> MakeBlueprintSCSViewportAdapter()
	{
		return MakeShared<FBlueprintSCSViewportAdapter>();
	}
}
