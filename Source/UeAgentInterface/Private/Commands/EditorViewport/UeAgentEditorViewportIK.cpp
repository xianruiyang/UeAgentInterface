// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commands/EditorViewport/UeAgentEditorViewportIK.h"

#include "Commands/EditorViewport/UeAgentEditorViewportResolver.h"
#include "LegacyMechanics/UeAgentLegacyMechanics.h"
#include "Utilities/UeAgentJsonDiagnostics.h"
#include "UeAgentInterfaceSettings.h"

#include "Animation/DebugSkelMeshComponent.h"
#include "AnimationEditorViewportClient.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/SkeletalMesh.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "IPersonaPreviewScene.h"
#include "IPersonaToolkit.h"
#include "Misc/App.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PersonaTabs.h"
#include "RenderingThread.h"
#include "RetargetEditor/IKRetargetEditor.h"
#include "RetargetEditor/IKRetargetEditorController.h"
#include "Retargeter/IKRetargetSettings.h"
#include "Retargeter/IKRetargeter.h"
#include "Rig/IKRigDefinition.h"
#include "RigEditor/IKRigEditorController.h"
#include "RigEditor/IKRigToolkit.h"
#include "SEditorViewport.h"
#include "ShowFlags.h"
#include "Slate/SceneViewport.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/AssetEditorToolkit.h"
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
		return UeAgentJsonDiagnostics::TryReadVectorObject(Obj, OutValue);
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

		static TSharedPtr<FJsonObject> MakeBoundsJson(const FBox& Box)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetObjectField(TEXT("min"), MakeVectorJson(Box.Min));
			Obj->SetObjectField(TEXT("max"), MakeVectorJson(Box.Max));
			Obj->SetObjectField(TEXT("center"), MakeVectorJson(Box.GetCenter()));
			Obj->SetObjectField(TEXT("extent"), MakeVectorJson(Box.GetExtent()));
			return Obj;
		}



		;





		static FString BoneDrawModeToString(const EBoneDrawMode::Type Mode)
		{
			switch (Mode)
			{
			case EBoneDrawMode::None: return TEXT("none");
			case EBoneDrawMode::Selected: return TEXT("selected");
			case EBoneDrawMode::SelectedAndParents: return TEXT("selected_and_parents");
			case EBoneDrawMode::SelectedAndChildren: return TEXT("selected_and_children");
			case EBoneDrawMode::SelectedAndParentsAndChildren: return TEXT("selected_and_parents_and_children");
			case EBoneDrawMode::All: return TEXT("all");
			default: return TEXT("unknown");
			}
		}

		static bool TryParseBoneDrawMode(const FString& InValue, EBoneDrawMode::Type& OutMode)
		{
			const FString Value = NormalizeToken(InValue);
			if (Value == TEXT("none")) { OutMode = EBoneDrawMode::None; return true; }
			if (Value == TEXT("selected")) { OutMode = EBoneDrawMode::Selected; return true; }
			if (Value == TEXT("selected_and_parents")) { OutMode = EBoneDrawMode::SelectedAndParents; return true; }
			if (Value == TEXT("selected_and_children")) { OutMode = EBoneDrawMode::SelectedAndChildren; return true; }
			if (Value == TEXT("selected_and_parents_and_children")) { OutMode = EBoneDrawMode::SelectedAndParentsAndChildren; return true; }
			if (Value == TEXT("all")) { OutMode = EBoneDrawMode::All; return true; }
			return false;
		}

		static FString LocalAxesModeToString(const ELocalAxesMode::Type Mode)
		{
			switch (Mode)
			{
			case ELocalAxesMode::None: return TEXT("none");
			case ELocalAxesMode::Selected: return TEXT("selected");
			case ELocalAxesMode::All: return TEXT("all");
			default: return TEXT("unknown");
			}
		}

		static bool TryParseLocalAxesMode(const FString& InValue, ELocalAxesMode::Type& OutMode)
		{
			const FString Value = NormalizeToken(InValue);
			if (Value == TEXT("none")) { OutMode = ELocalAxesMode::None; return true; }
			if (Value == TEXT("selected")) { OutMode = ELocalAxesMode::Selected; return true; }
			if (Value == TEXT("all")) { OutMode = ELocalAxesMode::All; return true; }
			return false;
		}

		static void ApplyToggleBoolOption(
			FAnimationViewportClient& Client,
			const TSharedPtr<FJsonObject>& Options,
			const FString& Key,
			bool (FAnimationViewportClient::*IsChecked)() const,
			void (FAnimationViewportClient::*Toggle)())
		{
			bool bValue = (Client.*IsChecked)();
			if (TryReadBoolOption(Options, Key, bValue) && (Client.*IsChecked)() != bValue)
			{
				(Client.*Toggle)();
			}
		}

		template <typename TObject>
		static TObject* LoadAssetByPath(FString AssetPath)
		{
			AssetPath.TrimStartAndEndInline();
			if (AssetPath.IsEmpty())
			{
				return nullptr;
			}
			if (TObject* Asset = LoadObject<TObject>(nullptr, *AssetPath))
			{
				return Asset;
			}
			if (!AssetPath.Contains(TEXT(".")))
			{
				const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *AssetPath, *FPackageName::GetShortName(AssetPath));
				return LoadObject<TObject>(nullptr, *ObjectPath);
			}
			return nullptr;
		}

		static bool IsIKRigAssetType(const FString& AssetType)
		{
			const FString Value = NormalizeToken(AssetType);
			return Value == TEXT("ik_rig")
				|| Value == TEXT("ikrig")
				|| Value == TEXT("ik_rig_definition")
				|| Value == TEXT("ikrigdefinition")
				|| Value == TEXT("uikrigdefinition");
		}

		static bool IsIKRetargeterAssetType(const FString& AssetType)
		{
			const FString Value = NormalizeToken(AssetType);
			return Value == TEXT("ik_retargeter")
				|| Value == TEXT("ikretargeter")
				|| Value == TEXT("retargeter")
				|| Value == TEXT("uikretargeter");
		}

		static UObject* ResolveIKAsset(const FString& AssetPath, const FString& AssetType, FString& OutCanonicalType, FString& OutError)
		{
			if (IsIKRigAssetType(AssetType))
			{
				if (UIKRigDefinition* Rig = LoadAssetByPath<UIKRigDefinition>(AssetPath))
				{
					OutCanonicalType = TEXT("ik_rig");
					return Rig;
				}
				OutError = TEXT("ik_rig_asset_not_found");
				return nullptr;
			}
			if (IsIKRetargeterAssetType(AssetType))
			{
				if (UIKRetargeter* Retargeter = LoadAssetByPath<UIKRetargeter>(AssetPath))
				{
					OutCanonicalType = TEXT("ik_retargeter");
					return Retargeter;
				}
				OutError = TEXT("ik_retargeter_asset_not_found");
				return nullptr;
			}
			if (!AssetType.IsEmpty())
			{
				OutError = TEXT("unsupported_ik_asset_type");
				return nullptr;
			}

			if (UIKRigDefinition* Rig = LoadAssetByPath<UIKRigDefinition>(AssetPath))
			{
				OutCanonicalType = TEXT("ik_rig");
				return Rig;
			}
			if (UIKRetargeter* Retargeter = LoadAssetByPath<UIKRetargeter>(AssetPath))
			{
				OutCanonicalType = TEXT("ik_retargeter");
				return Retargeter;
			}
			OutError = TEXT("ik_asset_not_found");
			return nullptr;
		}

		static bool OpenEditorIfNeeded(UObject* Asset, const bool bOpenIfNeeded, IAssetEditorInstance*& OutEditor, FString& OutError)
		{
			if (!Asset || !GEditor)
			{
				OutError = TEXT("invalid_ik_asset");
				return false;
			}
			UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
			if (!AssetEditorSubsystem)
			{
				OutError = TEXT("asset_editor_subsystem_unavailable");
				return false;
			}
			if (bOpenIfNeeded)
			{
				AssetEditorSubsystem->OpenEditorForAsset(Asset);
				if (FSlateApplication::IsInitialized())
				{
					FSlateApplication::Get().Tick();
				}
			}
			OutEditor = AssetEditorSubsystem->FindEditorForAsset(Asset, false);
			if (!OutEditor)
			{
				OutError = bOpenIfNeeded ? TEXT("ik_editor_open_failed") : TEXT("ik_editor_not_opened");
				return false;
			}
			return true;
		}

		static bool ToolkitEditsAsset(const FAssetEditorToolkit& Toolkit, const UObject* Asset)
		{
			if (!Asset)
			{
				return false;
			}
			if (const TArray<UObject*>* EditingObjects = Toolkit.GetObjectsCurrentlyBeingEdited())
			{
				for (UObject* EditingObject : *EditingObjects)
				{
					if (EditingObject == Asset)
					{
						return true;
					}
				}
			}
			return false;
		}

		static FAnimationViewportClient* FindPersonaViewportClient(const TSharedRef<IPersonaPreviewScene>& PreviewScene)
		{
			if (!GEditor)
			{
				return nullptr;
			}

			FPreviewScene* PreviewScenePtr = &PreviewScene.Get();
			for (FEditorViewportClient* Candidate : GEditor->GetAllViewportClients())
			{
				if (Candidate && Candidate->GetPreviewScene() == PreviewScenePtr)
				{
					return static_cast<FAnimationViewportClient*>(Candidate);
				}
			}
			return nullptr;
		}

		static TSharedPtr<SEditorViewport> ResolvePreviewViewportWidget(FAssetEditorToolkit& Toolkit)
		{
			const TSharedPtr<FTabManager> TabManager = Toolkit.GetAssociatedTabManager();
			if (!TabManager.IsValid())
			{
				return nullptr;
			}

			const TArray<FName> CandidateTabIds = { FPersonaTabs::PreviewViewportID, FPersonaTabs::PreviewViewport1ID, FPersonaTabs::PreviewViewport2ID, FPersonaTabs::PreviewViewport3ID };
			for (const FName& TabId : CandidateTabIds)
			{
				TSharedPtr<SDockTab> ViewportTab = TabManager->FindExistingLiveTab(FTabId(TabId));
				if (!ViewportTab.IsValid())
				{
					ViewportTab = TabManager->TryInvokeTab(FTabId(TabId), false);
				}
				if (ViewportTab.IsValid())
				{
					ViewportTab->ActivateInParent(ETabActivationCause::SetDirectly);
					ViewportTab->SlatePrepass();
					if (FSlateApplication::IsInitialized())
					{
						FSlateApplication::Get().Tick();
					}
					break;
				}
			}
			return nullptr;
		}

		enum class EIKAssetKind : uint8
		{
			IKRig,
			Retargeter
		};

		struct FIKRuntime
		{
			EIKAssetKind Kind = EIKAssetKind::IKRig;
			UIKRigDefinition* IKRigAsset = nullptr;
			UIKRetargeter* RetargeterAsset = nullptr;
			FIKRigEditorToolkit* IKRigEditor = nullptr;
			FIKRetargetEditor* RetargetEditor = nullptr;
			TSharedPtr<FIKRigEditorController> IKRigController;
			TSharedPtr<FIKRetargetEditorController> RetargetController;
			TSharedPtr<IPersonaToolkit> PersonaToolkit;
			TSharedPtr<IPersonaPreviewScene> PreviewScene;
			FAnimationViewportClient* Client = nullptr;
			UDebugSkelMeshComponent* ActiveComponent = nullptr;
			ERetargetSourceOrTarget Role = ERetargetSourceOrTarget::Target;
			FString RoleString = TEXT("primary");
		};

		static FString RoleToString(const ERetargetSourceOrTarget Role)
		{
			return Role == ERetargetSourceOrTarget::Source ? TEXT("source") : TEXT("target");
		}

		static bool TryParseRole(const FString& InValue, ERetargetSourceOrTarget& OutRole)
		{
			const FString Value = NormalizeToken(InValue);
			if (Value == TEXT("source") || Value == TEXT("src"))
			{
				OutRole = ERetargetSourceOrTarget::Source;
				return true;
			}
			if (Value == TEXT("target") || Value == TEXT("tgt") || Value == TEXT("primary"))
			{
				OutRole = ERetargetSourceOrTarget::Target;
				return true;
			}
			return false;
		}

		static ERetargetSourceOrTarget ResolveRequestedRole(
			const FResolvedViewport& Viewport,
			const TSharedPtr<FJsonObject>& Params,
			const FIKRetargetEditorController* Controller)
		{
			ERetargetSourceOrTarget Role = Controller ? Controller->GetSourceOrTarget() : ERetargetSourceOrTarget::Target;
			TryParseRole(Viewport.Target.ViewportRole, Role);
			FString Text;
			if (TryGetStringField(Params, TEXT("source_or_target"), Text)
				|| TryGetStringField(Params, TEXT("role"), Text)
				|| TryGetStringField(Params, TEXT("preview_role"), Text))
			{
				TryParseRole(Text, Role);
			}
			return Role;
		}

		static FString RetargeterModeToString(const ERetargeterOutputMode Mode)
		{
			switch (Mode)
			{
			case ERetargeterOutputMode::RunRetarget: return TEXT("run_retarget");
			case ERetargeterOutputMode::EditRetargetPose: return TEXT("edit_retarget_pose");
			default: return TEXT("unknown");
			}
		}

		static bool ResolveIKRuntime(
			const FResolvedViewport& Viewport,
			const TSharedPtr<FJsonObject>& Params,
			FIKRuntime& OutRuntime,
			FString& OutError)
		{
			UObject* Asset = Viewport.Asset;
			if (!Asset)
			{
				OutError = TEXT("ik_asset_missing");
				return false;
			}

			IAssetEditorInstance* EditorInstance = nullptr;
			if (!OpenEditorIfNeeded(Asset, false, EditorInstance, OutError))
			{
				return false;
			}
			FAssetEditorToolkit* Toolkit = static_cast<FAssetEditorToolkit*>(EditorInstance);
			if (!Toolkit || !ToolkitEditsAsset(*Toolkit, Asset))
			{
				OutError = TEXT("ik_editor_asset_mismatch");
				return false;
			}

			if (UIKRigDefinition* IKRigAsset = Cast<UIKRigDefinition>(Asset))
			{
				if (EditorInstance->GetEditorName() != FName(TEXT("IKRigEditor")))
				{
					OutError = FString::Printf(TEXT("unsupported_ik_rig_editor:%s"), *EditorInstance->GetEditorName().ToString());
					return false;
				}
				FIKRigEditorToolkit* IKRigEditor = static_cast<FIKRigEditorToolkit*>(Toolkit);
				TSharedRef<IPersonaToolkit> PersonaToolkit = IKRigEditor->GetPersonaToolkit();
				TSharedRef<IPersonaPreviewScene> PreviewScene = PersonaToolkit->GetPreviewScene();
				FAnimationViewportClient* Client = FindPersonaViewportClient(PreviewScene);
				if (!Client)
				{
					ResolvePreviewViewportWidget(*Toolkit);
					Client = FindPersonaViewportClient(PreviewScene);
				}
				if (!Client)
				{
					OutError = TEXT("ik_rig_viewport_not_found");
					return false;
				}

				OutRuntime.Kind = EIKAssetKind::IKRig;
				OutRuntime.IKRigAsset = IKRigAsset;
				OutRuntime.IKRigEditor = IKRigEditor;
				OutRuntime.IKRigController = IKRigEditor->GetController();
				OutRuntime.PersonaToolkit = PersonaToolkit;
				OutRuntime.PreviewScene = PreviewScene;
				OutRuntime.Client = Client;
				OutRuntime.ActiveComponent = OutRuntime.IKRigController.IsValid() ? OutRuntime.IKRigController->SkelMeshComponent : nullptr;
				if (!OutRuntime.ActiveComponent)
				{
					OutRuntime.ActiveComponent = PreviewScene->GetPreviewMeshComponent();
				}
				OutRuntime.RoleString = TEXT("primary");
				return true;
			}

			UIKRetargeter* RetargeterAsset = Cast<UIKRetargeter>(Asset);
			if (!RetargeterAsset)
			{
				OutError = TEXT("unsupported_ik_asset");
				return false;
			}
			if (EditorInstance->GetEditorName() != FName(TEXT("IKRetargetEditor")))
			{
				OutError = FString::Printf(TEXT("unsupported_ik_retargeter_editor:%s"), *EditorInstance->GetEditorName().ToString());
				return false;
			}
			FIKRetargetEditor* RetargetEditor = static_cast<FIKRetargetEditor*>(Toolkit);
			TSharedRef<IPersonaToolkit> PersonaToolkit = RetargetEditor->GetPersonaToolkit();
			TSharedRef<IPersonaPreviewScene> PreviewScene = PersonaToolkit->GetPreviewScene();
			FAnimationViewportClient* Client = FindPersonaViewportClient(PreviewScene);
			if (!Client)
			{
				ResolvePreviewViewportWidget(*Toolkit);
				Client = FindPersonaViewportClient(PreviewScene);
			}
			if (!Client)
			{
				OutError = TEXT("ik_retargeter_viewport_not_found");
				return false;
			}

			const TSharedRef<FIKRetargetEditorController> ControllerRef = RetargetEditor->GetController();
			FIKRetargetEditorController& Controller = ControllerRef.Get();
			const ERetargetSourceOrTarget Role = ResolveRequestedRole(Viewport, Params, &Controller);
			UDebugSkelMeshComponent* Component = Role == ERetargetSourceOrTarget::Source ? Controller.SourceSkelMeshComponent : Controller.TargetSkelMeshComponent;
			if (!Component)
			{
				Component = PreviewScene->GetPreviewMeshComponent();
			}

			OutRuntime.Kind = EIKAssetKind::Retargeter;
			OutRuntime.RetargeterAsset = RetargeterAsset;
			OutRuntime.RetargetEditor = RetargetEditor;
			OutRuntime.RetargetController = ControllerRef;
			OutRuntime.PersonaToolkit = PersonaToolkit;
			OutRuntime.PreviewScene = PreviewScene;
			OutRuntime.Client = Client;
			OutRuntime.ActiveComponent = Component;
			OutRuntime.Role = Role;
			OutRuntime.RoleString = RoleToString(Role);
			return true;
		}

		static FBox MakeComponentBoundsBox(const USkinnedMeshComponent* Component)
		{
			const FBoxSphereBounds Bounds = Component ? Component->Bounds : FBoxSphereBounds(EForceInit::ForceInit);
			return FBox(Bounds.Origin - Bounds.BoxExtent, Bounds.Origin + Bounds.BoxExtent);
		}

		static TSharedPtr<FJsonObject> MakeIKVisualOptionsJson(FAnimationViewportClient& Client, UDebugSkelMeshComponent* Component)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("view_mode"), ViewModeToString(Client.GetViewMode()));
			Obj->SetNumberField(TEXT("view_mode_value"), static_cast<int32>(Client.GetViewMode()));
			TMap<FString, bool> Overlays;
			Overlays.Add(TEXT("grid"), Client.IsSetShowGridChecked());
			Overlays.Add(TEXT("bounds"), Client.IsSetShowBoundsChecked());
			Overlays.Add(TEXT("bones"), Client.GetBoneDrawMode() != EBoneDrawMode::None);
			Overlays.Add(TEXT("local_axes"), Client.GetLocalAxesMode() != ELocalAxesMode::None);
			Overlays.Add(TEXT("sockets"), Component ? Component->bDrawSockets != 0 : false);
			Overlays.Add(TEXT("mesh_sockets"), Component ? Component->bMeshSocketsVisible != 0 : false);
			Overlays.Add(TEXT("skeleton_sockets"), Component ? Component->bSkeletonSocketsVisible != 0 : false);
			Overlays.Add(TEXT("normals"), Client.IsSetShowNormalsChecked());
			Overlays.Add(TEXT("tangents"), Client.IsSetShowTangentsChecked());
			Overlays.Add(TEXT("binormals"), Client.IsSetShowBinormalsChecked());
			Obj->SetObjectField(TEXT("overlays"), MakeBoolOptionsJson(Overlays));
			Obj->SetStringField(TEXT("bone_draw_mode"), BoneDrawModeToString(Client.GetBoneDrawMode()));
			Obj->SetStringField(TEXT("local_axes_mode"), LocalAxesModeToString(Client.GetLocalAxesMode()));
			Obj->SetBoolField(TEXT("realtime"), Client.IsRealtime());
			return Obj;
		}

		static TSharedPtr<FJsonObject> MakeComponentSummary(UDebugSkelMeshComponent* Component, const FString& Role)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("role"), Role);
			Obj->SetBoolField(TEXT("has_component"), Component != nullptr);
			if (!Component)
			{
				return Obj;
			}
			Obj->SetStringField(TEXT("component_name"), Component->GetName());
			if (USkeletalMesh* Mesh = Component->GetSkeletalMeshAsset())
			{
				Obj->SetStringField(TEXT("skeletal_mesh_asset_path"), Mesh->GetPathName());
			}
			const FBox Bounds = MakeComponentBoundsBox(Component);
			if (Bounds.IsValid)
			{
				Obj->SetStringField(TEXT("bounds_space"), TEXT("ik_preview_world"));
				Obj->SetObjectField(TEXT("bounds"), MakeBoundsJson(Bounds));
			}
			return Obj;
		}

		static void AppendGoals(const UIKRigDefinition* Rig, TSharedPtr<FJsonObject>& Obj, const FString& FieldName)
		{
			TArray<TSharedPtr<FJsonValue>> GoalsJson;
			if (Rig)
			{
				for (const UIKRigEffectorGoal* Goal : Rig->GetGoalArray())
				{
					if (!Goal)
					{
						continue;
					}
					TSharedPtr<FJsonObject> GoalObj = MakeShared<FJsonObject>();
					GoalObj->SetStringField(TEXT("goal_name"), Goal->GoalName.ToString());
					GoalObj->SetStringField(TEXT("bone_name"), Goal->BoneName.ToString());
					GoalObj->SetObjectField(TEXT("current_location"), MakeVectorJson(Goal->CurrentTransform.GetLocation()));
					GoalObj->SetObjectField(TEXT("initial_location"), MakeVectorJson(Goal->InitialTransform.GetLocation()));
					GoalsJson.Add(MakeShared<FJsonValueObject>(GoalObj));
				}
			}
			Obj->SetNumberField(FieldName + TEXT("_count"), GoalsJson.Num());
			Obj->SetArrayField(FieldName, GoalsJson);
		}

		static TSharedPtr<FJsonObject> MakeRoleRetargetState(
			const UIKRetargeter* Retargeter,
			const ERetargetSourceOrTarget Role,
			UDebugSkelMeshComponent* Component)
		{
			const FString RoleString = RoleToString(Role);
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("role"), RoleString);
			const UIKRigDefinition* Rig = Retargeter ? Retargeter->GetIKRig(Role) : nullptr;
			if (Rig)
			{
				Obj->SetStringField(TEXT("ik_rig_path"), Rig->GetPathName());
			}
#if WITH_EDITORONLY_DATA
			if (USkeletalMesh* PreviewMesh = Retargeter ? Retargeter->GetPreviewMesh(Role) : nullptr)
			{
				Obj->SetStringField(TEXT("preview_mesh_asset_path"), PreviewMesh->GetPathName());
			}
#endif
			if (Retargeter)
			{
				Obj->SetStringField(TEXT("current_pose"), Retargeter->GetCurrentRetargetPoseName(Role).ToString());
			}
			Obj->SetObjectField(TEXT("component"), MakeComponentSummary(Component, RoleString));
			AppendGoals(Rig, Obj, TEXT("goals"));
			return Obj;
		}

		static TSharedPtr<FJsonObject> MakeIKPreviewStateJson(const FIKRuntime& Runtime)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			if (Runtime.Kind == EIKAssetKind::IKRig)
			{
				Obj->SetStringField(TEXT("context"), TEXT("ik_rig"));
				Obj->SetStringField(TEXT("asset_path"), Runtime.IKRigAsset ? Runtime.IKRigAsset->GetPathName() : FString());
				Obj->SetStringField(TEXT("asset_type"), TEXT("ik_rig"));
				Obj->SetStringField(TEXT("viewport_role"), TEXT("primary"));
				if (USkeletalMesh* PreviewMesh = Runtime.IKRigAsset ? Runtime.IKRigAsset->GetPreviewMesh() : nullptr)
				{
					Obj->SetStringField(TEXT("preview_mesh_asset_path"), PreviewMesh->GetPathName());
				}
				Obj->SetObjectField(TEXT("component"), MakeComponentSummary(Runtime.ActiveComponent, TEXT("primary")));
				AppendGoals(Runtime.IKRigAsset, Obj, TEXT("goals"));
				return Obj;
			}

			Obj->SetStringField(TEXT("context"), TEXT("ik_retargeter"));
			Obj->SetStringField(TEXT("asset_path"), Runtime.RetargeterAsset ? Runtime.RetargeterAsset->GetPathName() : FString());
			Obj->SetStringField(TEXT("asset_type"), TEXT("ik_retargeter"));
			Obj->SetStringField(TEXT("viewport_role"), Runtime.RoleString);
			Obj->SetStringField(TEXT("source_or_target"), Runtime.RoleString);
			Obj->SetStringField(TEXT("active_role"), Runtime.RoleString);
			if (Runtime.RetargetController.IsValid())
			{
				Obj->SetStringField(TEXT("current_role"), RoleToString(Runtime.RetargetController->GetSourceOrTarget()));
				Obj->SetStringField(TEXT("output_mode"), RetargeterModeToString(Runtime.RetargetController->GetRetargeterMode()));
			}
			Obj->SetObjectField(TEXT("active_component"), MakeComponentSummary(Runtime.ActiveComponent, Runtime.RoleString));
			UDebugSkelMeshComponent* SourceComponent = Runtime.RetargetController.IsValid() ? Runtime.RetargetController->SourceSkelMeshComponent : nullptr;
			UDebugSkelMeshComponent* TargetComponent = Runtime.RetargetController.IsValid() ? Runtime.RetargetController->TargetSkelMeshComponent : nullptr;
			Obj->SetObjectField(TEXT("source"), MakeRoleRetargetState(Runtime.RetargeterAsset, ERetargetSourceOrTarget::Source, SourceComponent));
			Obj->SetObjectField(TEXT("target"), MakeRoleRetargetState(Runtime.RetargeterAsset, ERetargetSourceOrTarget::Target, TargetComponent));
			const UIKRigDefinition* ActiveRig = Runtime.RetargeterAsset ? Runtime.RetargeterAsset->GetIKRig(Runtime.Role) : nullptr;
			AppendGoals(ActiveRig, Obj, TEXT("role_goals"));
			return Obj;
		}

		static bool ResolveGoalFocus(
			const UIKRigDefinition* Rig,
			UDebugSkelMeshComponent* Component,
			const TSharedPtr<FJsonObject>& FocusObj,
			FVector& OutPoint,
			TSharedPtr<FJsonObject>& OutGoalData,
			FString& OutError)
		{
			if (!Rig || !Component)
			{
				OutError = TEXT("ik_goal_focus_context_missing");
				return false;
			}
			FString GoalName;
			if (!TryGetStringField(FocusObj, TEXT("goal"), GoalName)
				&& !TryGetStringField(FocusObj, TEXT("goal_name"), GoalName)
				&& !TryGetStringField(FocusObj, TEXT("name"), GoalName))
			{
				OutError = TEXT("missing_goal_name");
				return false;
			}

			const UIKRigEffectorGoal* FoundGoal = nullptr;
			for (const UIKRigEffectorGoal* Goal : Rig->GetGoalArray())
			{
				if (Goal && Goal->GoalName == FName(*GoalName))
				{
					FoundGoal = Goal;
					break;
				}
			}
			if (!FoundGoal)
			{
				OutError = TEXT("ik_goal_not_found");
				return false;
			}

			const FTransform PreviewWorldTransform = FoundGoal->CurrentTransform * Component->GetComponentTransform();
			OutPoint = PreviewWorldTransform.GetLocation();
			OutGoalData = MakeShared<FJsonObject>();
			OutGoalData->SetStringField(TEXT("goal_name"), FoundGoal->GoalName.ToString());
			OutGoalData->SetStringField(TEXT("goal_bone"), FoundGoal->BoneName.ToString());
			OutGoalData->SetStringField(TEXT("goal_space"), TEXT("ik_rig_global"));
			OutGoalData->SetObjectField(TEXT("goal_current_location"), MakeVectorJson(FoundGoal->CurrentTransform.GetLocation()));
			OutGoalData->SetObjectField(TEXT("preview_world_location"), MakeVectorJson(OutPoint));
			return true;
		}

		static bool IsReadOnlyPreviewStateKey(const FString& Key)
		{
			const FString Value = NormalizeToken(Key);
			return Value == TEXT("context")
				|| Value == TEXT("asset_path")
				|| Value == TEXT("asset_type")
				|| Value == TEXT("viewport_role")
				|| Value == TEXT("source_or_target")
				|| Value == TEXT("active_role")
				|| Value == TEXT("current_role")
				|| Value == TEXT("output_mode")
				|| Value == TEXT("source")
				|| Value == TEXT("target")
				|| Value == TEXT("active_component")
				|| Value == TEXT("component")
				|| Value == TEXT("preview_mesh_asset_path")
				|| Value == TEXT("goals")
				|| Value == TEXT("goals_count")
				|| Value == TEXT("role_goals")
				|| Value == TEXT("role_goals_count");
		}

		class FIKViewportAdapter final : public IViewportAdapter
		{
		public:
			virtual FString GetName() const override
			{
				return TEXT("ik");
			}

			virtual bool CanResolve(const FResolveRequest& Request) const override
			{
				if (Request.Target.Scope != TEXT("asset_editor"))
				{
					return false;
				}
				return Request.Target.AssetType.IsEmpty()
					|| IsIKRigAssetType(Request.Target.AssetType)
					|| IsIKRetargeterAssetType(Request.Target.AssetType);
			}

			virtual bool Resolve(const FResolveRequest& Request, FResolvedViewport& OutViewport, FString& OutError) const override
			{
				if (Request.Target.AssetPath.IsEmpty())
				{
					OutError = TEXT("missing_asset_path");
					return false;
				}
				FString CanonicalType;
				UObject* Asset = ResolveIKAsset(Request.Target.AssetPath, Request.Target.AssetType, CanonicalType, OutError);
				if (!Asset)
				{
					return false;
				}

				IAssetEditorInstance* EditorInstance = nullptr;
				if (CanonicalType == TEXT("ik_retargeter") && Request.Target.bOpenEditorIfNeeded)
				{
					FString ExistingEditorError;
					if (!OpenEditorIfNeeded(Asset, false, EditorInstance, ExistingEditorError))
					{
						OutError = FApp::IsUnattended()
							? TEXT("ik_retargeter_editor_open_unsupported_unattended")
							: TEXT("ik_retargeter_editor_open_requires_existing_editor");
						return false;
					}
				}
				else if (!OpenEditorIfNeeded(Asset, Request.Target.bOpenEditorIfNeeded, EditorInstance, OutError))
				{
					return false;
				}
				FAssetEditorToolkit* Toolkit = static_cast<FAssetEditorToolkit*>(EditorInstance);
				if (!Toolkit || !ToolkitEditsAsset(*Toolkit, Asset))
				{
					OutError = TEXT("ik_editor_asset_mismatch");
					return false;
				}

				TSharedPtr<IPersonaToolkit> PersonaToolkit;
				if (CanonicalType == TEXT("ik_rig"))
				{
					if (EditorInstance->GetEditorName() != FName(TEXT("IKRigEditor")))
					{
						OutError = FString::Printf(TEXT("unsupported_ik_rig_editor:%s"), *EditorInstance->GetEditorName().ToString());
						return false;
					}
					PersonaToolkit = static_cast<FIKRigEditorToolkit*>(Toolkit)->GetPersonaToolkit();
				}
				else
				{
					if (EditorInstance->GetEditorName() != FName(TEXT("IKRetargetEditor")))
					{
						OutError = FString::Printf(TEXT("unsupported_ik_retargeter_editor:%s"), *EditorInstance->GetEditorName().ToString());
						return false;
					}
					PersonaToolkit = static_cast<FIKRetargetEditor*>(Toolkit)->GetPersonaToolkit();
				}

				const TSharedRef<IPersonaPreviewScene> PreviewScene = PersonaToolkit.ToSharedRef()->GetPreviewScene();
				FAnimationViewportClient* Client = FindPersonaViewportClient(PreviewScene);
				if (!Client)
				{
					ResolvePreviewViewportWidget(*Toolkit);
					Client = FindPersonaViewportClient(PreviewScene);
				}
				if (!Client)
				{
					OutError = TEXT("ik_preview_viewport_not_found");
					return false;
				}

				OutViewport.Target = Request.Target;
				OutViewport.Target.Scope = TEXT("asset_editor");
				OutViewport.Target.AssetPath = Asset->GetPathName();
				OutViewport.Target.AssetType = CanonicalType;
				OutViewport.Target.ViewportRole = NormalizeToken(Request.Target.ViewportRole.IsEmpty() ? TEXT("primary") : Request.Target.ViewportRole);
				if (CanonicalType == TEXT("ik_rig"))
				{
					OutViewport.Target.ViewportRole = TEXT("primary");
				}
				else if (OutViewport.Target.ViewportRole == TEXT("primary"))
				{
					OutViewport.Target.ViewportRole = TEXT("target");
				}
				OutViewport.Target.ViewportId = FString::Printf(TEXT("asset:%s#%s_%s"), *Asset->GetPathName(), *CanonicalType, *OutViewport.Target.ViewportRole);
				OutViewport.AdapterName = GetName();
				OutViewport.ClientClass = TEXT("FAnimationViewportClient");
				OutViewport.ViewportClient = Client;
				OutViewport.Asset = Asset;
				return true;
			}

			virtual bool ReadState(const FResolvedViewport& Viewport, FViewportState& OutState, FString& OutError) const override
			{
				FIKRuntime Runtime;
				if (!ResolveIKRuntime(Viewport, MakeShared<FJsonObject>(), Runtime, OutError))
				{
					return false;
				}

				OutState.Target = Viewport.Target;
				OutState.Target.ViewportRole = Runtime.RoleString;
				if (Runtime.Kind == EIKAssetKind::IKRig)
				{
					OutState.Target.ViewportRole = TEXT("primary");
				}
				OutState.ClientClass = TEXT("FAnimationViewportClient");
				OutState.Adapter = GetName();
				OutState.Camera.Location = Runtime.Client->GetViewLocation();
				OutState.Camera.Rotation = Runtime.Client->GetViewRotation();
				OutState.Camera.Fov = Runtime.Client->ViewFOV;
				OutState.Camera.Space = TEXT("ik_preview_world");
				OutState.Camera.Projection = Runtime.Client->ViewportType == LVT_Perspective ? TEXT("perspective") : TEXT("orthographic");
				OutState.Camera.bValid = true;
				OutState.Capabilities.bCamera = true;
				OutState.Capabilities.bVisualOptions = true;
				OutState.Capabilities.bPreviewState = true;

				OutState.Capabilities.FocusModes = { TEXT("preview_component_bounds"), TEXT("asset_bounds"), TEXT("bounds"), TEXT("selection"), TEXT("bone"), TEXT("goal"), TEXT("custom_bounds"), TEXT("custom_point") };

				OutState.Capabilities.VisualOptionKeys = { TEXT("view_mode"), TEXT("grid"), TEXT("bounds"), TEXT("bones"), TEXT("bone_draw_mode"), TEXT("local_axes"), TEXT("local_axes_mode"), TEXT("sockets"), TEXT("mesh_sockets"), TEXT("skeleton_sockets"), TEXT("normals"), TEXT("tangents"), TEXT("binormals"), TEXT("realtime") };
				OutState.Capabilities.PreviewStateKeys = { TEXT("source_or_target"), TEXT("current_role"), TEXT("output_mode"), TEXT("current_pose"), TEXT("goals") };
				OutState.VisualOptions = MakeIKVisualOptionsJson(*Runtime.Client, Runtime.ActiveComponent);
				OutState.PreviewState = MakeIKPreviewStateJson(Runtime);
				return true;
			}

			virtual bool WriteCamera(const FResolvedViewport& Viewport, const FViewportCamera& Camera, FString& OutError) const override
			{
				FIKRuntime Runtime;
				if (!ResolveIKRuntime(Viewport, MakeShared<FJsonObject>(), Runtime, OutError))
				{
					return false;
				}
				Runtime.Client->SetCameraFollowMode(EAnimationViewportCameraFollowMode::None);
				Runtime.Client->SetViewLocation(Camera.Location);
				Runtime.Client->SetViewRotation(Camera.Rotation);
				Runtime.Client->ViewFOV = FMath::Clamp(Camera.Fov, 5.0f, 170.0f);
				Runtime.Client->Invalidate();
				if (Runtime.Client->Viewport)
				{
					Runtime.Client->Viewport->Invalidate();
				}
				return true;
			}

			virtual bool Focus(const FResolvedViewport& Viewport, const TSharedPtr<FJsonObject>& FocusObj, TSharedPtr<FJsonObject>& OutData, FString& OutError) const override
			{
				FIKRuntime Runtime;
				if (!ResolveIKRuntime(Viewport, FocusObj, Runtime, OutError))
				{
					return false;
				}
				if (!Runtime.ActiveComponent)
				{
					OutError = TEXT("missing_ik_preview_component");
					return false;
				}

				FString Mode = TEXT("preview_component_bounds");
				TryGetStringField(FocusObj, TEXT("mode"), Mode);
				Mode = NormalizeToken(Mode);

				FBox FocusBox(ForceInit);
				FVector FocusPoint = FVector::ZeroVector;
				bool bUsePoint = false;
				TSharedPtr<FJsonObject> GoalData;
				if (Mode == TEXT("preview_component_bounds") || Mode == TEXT("asset_bounds") || Mode == TEXT("bounds") || Mode == TEXT("selection"))
				{
					FocusBox = MakeComponentBoundsBox(Runtime.ActiveComponent);
					if (Mode == TEXT("selection"))
					{
						Mode = TEXT("preview_component_bounds");
					}
				}
				else if (Mode == TEXT("bone"))
				{
					FString BoneName;
					if (!TryGetStringField(FocusObj, TEXT("bone"), BoneName) && !TryGetStringField(FocusObj, TEXT("bone_name"), BoneName))
					{
						OutError = TEXT("missing_bone_name");
						return false;
					}
					const int32 BoneIndex = Runtime.ActiveComponent->GetBoneIndex(FName(*BoneName));
					if (BoneIndex == INDEX_NONE)
					{
						OutError = TEXT("ik_bone_not_found");
						return false;
					}
					FocusPoint = Runtime.ActiveComponent->GetBoneTransform(BoneIndex).GetLocation();
					bUsePoint = true;
				}
				else if (Mode == TEXT("goal"))
				{
					const UIKRigDefinition* Rig = Runtime.Kind == EIKAssetKind::IKRig
						? Runtime.IKRigAsset
						: (Runtime.RetargeterAsset ? Runtime.RetargeterAsset->GetIKRig(Runtime.Role) : nullptr);
					if (!ResolveGoalFocus(Rig, Runtime.ActiveComponent, FocusObj, FocusPoint, GoalData, OutError))
					{
						return false;
					}
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
					OutError = TEXT("unsupported_focus_mode");
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

				FSphere FocusSphere(FocusBox.GetCenter(), FMath::Max(FocusBox.GetExtent().Size(), 1.0));
				Runtime.Client->FocusViewportOnSphere(FocusSphere, bInstant);
				Runtime.Client->SetLookAtLocation(FocusBox.GetCenter(), false);
				Runtime.Client->Invalidate();
				if (Runtime.Client->Viewport)
				{
					Runtime.Client->Viewport->Invalidate();
				}
				Runtime.PreviewScene->InvalidateViews();

				OutData = MakeShared<FJsonObject>();
				OutData->SetStringField(TEXT("mode"), Mode);
				OutData->SetStringField(TEXT("bounds_space"), TEXT("ik_preview_world"));
				OutData->SetStringField(TEXT("source_or_target"), Runtime.RoleString);
				OutData->SetBoolField(TEXT("instant"), bInstant);
				OutData->SetObjectField(TEXT("bounds"), MakeBoundsJson(FocusBox));
				if (GoalData.IsValid())
				{
					OutData->SetObjectField(TEXT("goal"), GoalData);
				}
				return true;
			}

			virtual bool ApplyVisualOptions(const FResolvedViewport& Viewport, const TSharedPtr<FJsonObject>& Options, TSharedPtr<FJsonObject>& OutData, FString& OutError) const override
			{
				FIKRuntime Runtime;
				if (!ResolveIKRuntime(Viewport, Options, Runtime, OutError))
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
						Runtime.Client->SetViewMode(ViewMode);
					}
				}

				bool bGrid = Runtime.Client->IsSetShowGridChecked();
				if (TryReadBoolOption(Options, TEXT("grid"), bGrid) && Runtime.Client->IsSetShowGridChecked() != bGrid)
				{
					Runtime.Client->SetShowGrid();
				}

				bool bBounds = Runtime.Client->IsSetShowBoundsChecked();
				if (TryReadBoolOption(Options, TEXT("bounds"), bBounds))
				{
					Runtime.Client->SetShowBounds(bBounds);
				}

				bool bBones = Runtime.Client->GetBoneDrawMode() != EBoneDrawMode::None;
				if (TryReadBoolOption(Options, TEXT("bones"), bBones))
				{
					Runtime.Client->SetBoneDrawMode(bBones ? EBoneDrawMode::All : EBoneDrawMode::None);
				}
				FString BoneDrawModeText;
				if (TryGetStringField(Options, TEXT("bone_draw_mode"), BoneDrawModeText))
				{
					EBoneDrawMode::Type BoneDrawMode = EBoneDrawMode::None;
					if (!TryParseBoneDrawMode(BoneDrawModeText, BoneDrawMode))
					{
						Unsupported.Add(TEXT("bone_draw_mode"));
					}
					else
					{
						Runtime.Client->SetBoneDrawMode(BoneDrawMode);
					}
				}

				bool bLocalAxes = Runtime.Client->GetLocalAxesMode() != ELocalAxesMode::None;
				if (TryReadBoolOption(Options, TEXT("local_axes"), bLocalAxes))
				{
					Runtime.Client->SetLocalAxesMode(bLocalAxes ? ELocalAxesMode::All : ELocalAxesMode::None);
				}
				FString LocalAxesModeText;
				if (TryGetStringField(Options, TEXT("local_axes_mode"), LocalAxesModeText))
				{
					ELocalAxesMode::Type LocalAxesMode = ELocalAxesMode::None;
					if (!TryParseLocalAxesMode(LocalAxesModeText, LocalAxesMode))
					{
						Unsupported.Add(TEXT("local_axes_mode"));
					}
					else
					{
						Runtime.Client->SetLocalAxesMode(LocalAxesMode);
					}
				}

				if (Runtime.ActiveComponent)
				{
					bool bSockets = Runtime.ActiveComponent->bDrawSockets != 0;
					if (TryReadBoolOption(Options, TEXT("sockets"), bSockets))
					{
						Runtime.ActiveComponent->bDrawSockets = bSockets;
						if (bSockets && !Runtime.ActiveComponent->bMeshSocketsVisible && !Runtime.ActiveComponent->bSkeletonSocketsVisible)
						{
							Runtime.ActiveComponent->bMeshSocketsVisible = true;
							Runtime.ActiveComponent->bSkeletonSocketsVisible = true;
						}
						Runtime.ActiveComponent->MarkRenderStateDirty();
					}

					bool bMeshSockets = Runtime.ActiveComponent->bMeshSocketsVisible != 0;
					if (TryReadBoolOption(Options, TEXT("mesh_sockets"), bMeshSockets))
					{
						Runtime.ActiveComponent->bMeshSocketsVisible = bMeshSockets;
						if (bMeshSockets)
						{
							Runtime.ActiveComponent->bDrawSockets = true;
						}
						Runtime.ActiveComponent->MarkRenderStateDirty();
					}

					bool bSkeletonSockets = Runtime.ActiveComponent->bSkeletonSocketsVisible != 0;
					if (TryReadBoolOption(Options, TEXT("skeleton_sockets"), bSkeletonSockets))
					{
						Runtime.ActiveComponent->bSkeletonSocketsVisible = bSkeletonSockets;
						if (bSkeletonSockets)
						{
							Runtime.ActiveComponent->bDrawSockets = true;
						}
						Runtime.ActiveComponent->MarkRenderStateDirty();
					}
				}

				ApplyToggleBoolOption(*Runtime.Client, Options, TEXT("normals"), &FAnimationViewportClient::IsSetShowNormalsChecked, &FAnimationViewportClient::ToggleShowNormals);
				ApplyToggleBoolOption(*Runtime.Client, Options, TEXT("tangents"), &FAnimationViewportClient::IsSetShowTangentsChecked, &FAnimationViewportClient::ToggleShowTangents);
				ApplyToggleBoolOption(*Runtime.Client, Options, TEXT("binormals"), &FAnimationViewportClient::IsSetShowBinormalsChecked, &FAnimationViewportClient::ToggleShowBinormals);

				bool bRealtime = Runtime.Client->IsRealtime();
				if (TryGetBoolField(Options, TEXT("realtime"), bRealtime))
				{
					Runtime.Client->SetRealtime(bRealtime);
				}

				Runtime.Client->Invalidate();
				if (Runtime.Client->Viewport)
				{
					Runtime.Client->Viewport->Invalidate();
				}
				Runtime.PreviewScene->InvalidateViews();

				OutData = MakeIKVisualOptionsJson(*Runtime.Client, Runtime.ActiveComponent);
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
				FIKRuntime Runtime;
				if (!ResolveIKRuntime(Viewport, PreviewState, Runtime, OutError))
				{
					return false;
				}
				OutData = MakeIKPreviewStateJson(Runtime);
				if (!HasUserFields(PreviewState))
				{
					OutData->SetBoolField(TEXT("applied"), false);
					OutData->SetStringField(TEXT("reason"), TEXT("empty_preview_state"));
					return true;
				}

				TArray<FString> Unsupported;
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : PreviewState->Values)
				{
					if (!IsReadOnlyPreviewStateKey(Pair.Key))
					{
						Unsupported.Add(Pair.Key);
					}
				}
				if (Unsupported.Num() == 0)
				{
					OutData->SetBoolField(TEXT("applied"), false);
					OutData->SetStringField(TEXT("reason"), TEXT("read_only_preview_state"));
					return true;
				}
				OutData->SetBoolField(TEXT("applied"), false);
				OutData->SetArrayField(TEXT("unsupported_options"), MakeStringArrayJson(Unsupported));
				bool bIgnoreUnsupported = false;
				TryGetBoolField(PreviewState, TEXT("ignore_unsupported"), bIgnoreUnsupported);
				if (!bIgnoreUnsupported)
				{
					OutError = TEXT("editor_viewport_preview_state_not_supported");
					return false;
				}
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
					OutError = TEXT("invalid_snapshot");
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

	TSharedRef<IViewportAdapter> MakeIKViewportAdapter()
	{
		return MakeShared<FIKViewportAdapter>();
	}
}
