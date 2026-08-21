// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commands/EditorViewport/UeAgentEditorViewportPhysicsAsset.h"

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
#include "IHasPersonaToolkit.h"
#include "IPhysicsAssetEditor.h"
#include "IPersonaPreviewScene.h"
#include "IPersonaToolkit.h"
#include "Misc/App.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PersonaTabs.h"
#include "PhysicsAssetRenderUtils.h"
#include "PhysicsEngine/AggregateGeom.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "Preferences/PhysicsAssetEditorOptions.h"
#include "RenderingThread.h"
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
		static constexpr const TCHAR* PhysicsAssetCameraSpace = TEXT("physics_asset_preview_world");

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

		static bool TryGetNestedObject(const TSharedPtr<FJsonObject>& Obj, const FString& Key, TSharedPtr<FJsonObject>& OutNested)
		{
			if (TryGetObjectField(Obj, Key, OutNested))
			{
				return true;
			}
			return false;
		}

		static bool TryReadBoolOption(const TSharedPtr<FJsonObject>& Options, const FString& Key, bool& OutValue)
		{
			if (TryGetBoolField(Options, Key, OutValue))
			{
				return true;
			}
			TSharedPtr<FJsonObject> Nested;
			if (TryGetNestedObject(Options, TEXT("overlays"), Nested) && TryGetBoolField(Nested, Key, OutValue))
			{
				return true;
			}
			if (TryGetNestedObject(Options, TEXT("physics_asset"), Nested) && TryGetBoolField(Nested, Key, OutValue))
			{
				return true;
			}
			return false;
		}

		static bool TryReadStringOption(const TSharedPtr<FJsonObject>& Options, const FString& Key, FString& OutValue)
		{
			if (TryGetStringField(Options, Key, OutValue))
			{
				return true;
			}
			TSharedPtr<FJsonObject> Nested;
			if (TryGetNestedObject(Options, TEXT("overlays"), Nested) && TryGetStringField(Nested, Key, OutValue))
			{
				return true;
			}
			if (TryGetNestedObject(Options, TEXT("physics_asset"), Nested) && TryGetStringField(Nested, Key, OutValue))
			{
				return true;
			}
			return false;
		}

		static bool TryReadNumberOption(const TSharedPtr<FJsonObject>& Options, const FString& Key, double& OutValue)
		{
			if (TryGetNumberField(Options, Key, OutValue))
			{
				return true;
			}
			TSharedPtr<FJsonObject> Nested;
			if (TryGetNestedObject(Options, TEXT("physics_asset"), Nested) && TryGetNumberField(Nested, Key, OutValue))
			{
				return true;
			}
			return false;
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

		static FString CollisionViewModeToString(const EPhysicsAssetEditorCollisionViewMode Mode)
		{
			switch (Mode)
			{
			case EPhysicsAssetEditorCollisionViewMode::Solid: return TEXT("solid");
			case EPhysicsAssetEditorCollisionViewMode::Wireframe: return TEXT("wireframe");
			case EPhysicsAssetEditorCollisionViewMode::SolidWireframe: return TEXT("solid_wireframe");
			case EPhysicsAssetEditorCollisionViewMode::None: return TEXT("none");
			default: return TEXT("unknown");
			}
		}

		static bool TryParseCollisionViewMode(const FString& InValue, EPhysicsAssetEditorCollisionViewMode& OutMode)
		{
			const FString Value = NormalizeToken(InValue);
			if (Value == TEXT("solid")) { OutMode = EPhysicsAssetEditorCollisionViewMode::Solid; return true; }
			if (Value == TEXT("wireframe")) { OutMode = EPhysicsAssetEditorCollisionViewMode::Wireframe; return true; }
			if (Value == TEXT("solid_wireframe") || Value == TEXT("solidwireframe")) { OutMode = EPhysicsAssetEditorCollisionViewMode::SolidWireframe; return true; }
			if (Value == TEXT("none") || Value == TEXT("hidden") || Value == TEXT("off")) { OutMode = EPhysicsAssetEditorCollisionViewMode::None; return true; }
			return false;
		}

		static FString ConstraintViewModeToString(const EPhysicsAssetEditorConstraintViewMode Mode)
		{
			switch (Mode)
			{
			case EPhysicsAssetEditorConstraintViewMode::None: return TEXT("none");
			case EPhysicsAssetEditorConstraintViewMode::AllPositions: return TEXT("all_positions");
			case EPhysicsAssetEditorConstraintViewMode::AllLimits: return TEXT("all_limits");
			default: return TEXT("unknown");
			}
		}

		static bool TryParseConstraintViewMode(const FString& InValue, EPhysicsAssetEditorConstraintViewMode& OutMode)
		{
			const FString Value = NormalizeToken(InValue);
			if (Value == TEXT("none") || Value == TEXT("hidden") || Value == TEXT("off")) { OutMode = EPhysicsAssetEditorConstraintViewMode::None; return true; }
			if (Value == TEXT("all_positions") || Value == TEXT("positions")) { OutMode = EPhysicsAssetEditorConstraintViewMode::AllPositions; return true; }
			if (Value == TEXT("all_limits") || Value == TEXT("limits")) { OutMode = EPhysicsAssetEditorConstraintViewMode::AllLimits; return true; }
			return false;
		}

		static FString CenterOfMassViewModeToString(const EPhysicsAssetEditorCenterOfMassViewMode Mode)
		{
			switch (Mode)
			{
			case EPhysicsAssetEditorCenterOfMassViewMode::All: return TEXT("all");
			case EPhysicsAssetEditorCenterOfMassViewMode::Selected: return TEXT("selected");
			case EPhysicsAssetEditorCenterOfMassViewMode::None: return TEXT("none");
			default: return TEXT("unknown");
			}
		}

		static bool TryParseCenterOfMassViewMode(const FString& InValue, EPhysicsAssetEditorCenterOfMassViewMode& OutMode)
		{
			const FString Value = NormalizeToken(InValue);
			if (Value == TEXT("all") || Value == TEXT("on")) { OutMode = EPhysicsAssetEditorCenterOfMassViewMode::All; return true; }
			if (Value == TEXT("selected")) { OutMode = EPhysicsAssetEditorCenterOfMassViewMode::Selected; return true; }
			if (Value == TEXT("none") || Value == TEXT("hidden") || Value == TEXT("off")) { OutMode = EPhysicsAssetEditorCenterOfMassViewMode::None; return true; }
			return false;
		}

		static FString PhysicsTypeToString(const EPhysicsType Type)
		{
			switch (Type)
			{
			case PhysType_Default: return TEXT("default");
			case PhysType_Kinematic: return TEXT("kinematic");
			case PhysType_Simulated: return TEXT("simulated");
			default: return TEXT("unknown");
			}
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

		static bool IsPhysicsAssetType(const FString& AssetType)
		{
			const FString Value = NormalizeToken(AssetType);
			return Value == TEXT("physics_asset")
				|| Value == TEXT("physicsasset")
				|| Value == TEXT("uphysicsasset")
				|| Value == TEXT("phat");
		}

		static UPhysicsAsset* ResolvePhysicsAsset(const FString& AssetPath, const FString& AssetType, FString& OutError)
		{
			if (!AssetType.IsEmpty() && !IsPhysicsAssetType(AssetType))
			{
				OutError = TEXT("unsupported_physics_asset_type");
				return nullptr;
			}
			UPhysicsAsset* Asset = LoadAssetByPath<UPhysicsAsset>(AssetPath);
			if (!Asset)
			{
				OutError = TEXT("physics_asset_not_found");
				return nullptr;
			}
			return Asset;
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

		static bool OpenEditorIfNeeded(UObject* Asset, const bool bOpenIfNeeded, IAssetEditorInstance*& OutEditor, FString& OutError)
		{
			if (!Asset || !GEditor)
			{
				OutError = TEXT("invalid_physics_asset");
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
				OutError = bOpenIfNeeded ? TEXT("physics_asset_editor_open_failed") : TEXT("physics_asset_editor_not_opened");
				return false;
			}
			return true;
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

		static void ResolvePreviewViewportWidget(FAssetEditorToolkit& Toolkit)
		{
			const TSharedPtr<FTabManager> TabManager = Toolkit.GetAssociatedTabManager();
			if (!TabManager.IsValid())
			{
				return;
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
		}

		struct FPhysicsAssetRuntime
		{
			UPhysicsAsset* PhysicsAsset = nullptr;
			FAssetEditorToolkit* Toolkit = nullptr;
			TSharedPtr<IPersonaToolkit> PersonaToolkit;
			TSharedPtr<IPersonaPreviewScene> PreviewScene;
			FAnimationViewportClient* Client = nullptr;
			UDebugSkelMeshComponent* PreviewComponent = nullptr;
			FPhysicsAssetRenderSettings* RenderSettings = nullptr;
		};

		static bool ResolveRuntime(const FResolvedViewport& Viewport, FPhysicsAssetRuntime& OutRuntime, FString& OutError)
		{
			UPhysicsAsset* PhysicsAsset = Cast<UPhysicsAsset>(Viewport.Asset);
			if (!PhysicsAsset)
			{
				OutError = TEXT("physics_asset_missing");
				return false;
			}

			IAssetEditorInstance* EditorInstance = nullptr;
			if (!OpenEditorIfNeeded(PhysicsAsset, false, EditorInstance, OutError))
			{
				return false;
			}
			if (EditorInstance->GetEditorName() != FName(TEXT("PhysicsAssetEditor")))
			{
				OutError = FString::Printf(TEXT("unsupported_physics_asset_editor:%s"), *EditorInstance->GetEditorName().ToString());
				return false;
			}
			FAssetEditorToolkit* Toolkit = static_cast<FAssetEditorToolkit*>(EditorInstance);
			if (!Toolkit || !ToolkitEditsAsset(*Toolkit, PhysicsAsset))
			{
				OutError = TEXT("physics_asset_editor_asset_mismatch");
				return false;
			}

			IPhysicsAssetEditor* PhysicsAssetEditor = static_cast<IPhysicsAssetEditor*>(Toolkit);
			TSharedRef<IPersonaToolkit> PersonaToolkit = PhysicsAssetEditor->GetPersonaToolkit();
			TSharedRef<IPersonaPreviewScene> PreviewScene = PersonaToolkit->GetPreviewScene();
			FAnimationViewportClient* Client = FindPersonaViewportClient(PreviewScene);
			if (!Client)
			{
				ResolvePreviewViewportWidget(*Toolkit);
				Client = FindPersonaViewportClient(PreviewScene);
			}
			if (!Client)
			{
				OutError = TEXT("physics_asset_preview_viewport_not_found");
				return false;
			}

			OutRuntime.PhysicsAsset = PhysicsAsset;
			OutRuntime.Toolkit = Toolkit;
			OutRuntime.PersonaToolkit = PersonaToolkit;
			OutRuntime.PreviewScene = PreviewScene;
			OutRuntime.Client = Client;
			OutRuntime.PreviewComponent = PreviewScene->GetPreviewMeshComponent();
			OutRuntime.RenderSettings = UPhysicsAssetRenderUtilities::GetSettings(PhysicsAsset);
			return true;
		}

		static FBox MakeComponentBoundsBox(const USkinnedMeshComponent* Component)
		{
			const FBoxSphereBounds Bounds = Component ? Component->Bounds : FBoxSphereBounds(EForceInit::ForceInit);
			return FBox(Bounds.Origin - Bounds.BoxExtent, Bounds.Origin + Bounds.BoxExtent);
		}

		static int32 GetPrimitiveCount(const FKAggregateGeom& AggGeom)
		{
			return AggGeom.SphereElems.Num()
				+ AggGeom.BoxElems.Num()
				+ AggGeom.SphylElems.Num()
				+ AggGeom.ConvexElems.Num()
				+ AggGeom.TaperedCapsuleElems.Num()
				+ AggGeom.LevelSetElems.Num()
				+ AggGeom.SkinnedLevelSetElems.Num()
				+ AggGeom.MLLevelSetElems.Num()
				+ AggGeom.SkinnedTriangleMeshElems.Num();
		}

		static TSharedPtr<FJsonObject> MakeBodySummaryJson(const UPhysicsAsset& PhysicsAsset, const int32 BodyIndex)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetNumberField(TEXT("index"), BodyIndex);
			if (!PhysicsAsset.SkeletalBodySetups.IsValidIndex(BodyIndex) || !PhysicsAsset.SkeletalBodySetups[BodyIndex])
			{
				Obj->SetBoolField(TEXT("valid"), false);
				return Obj;
			}
			const USkeletalBodySetup* BodySetup = PhysicsAsset.SkeletalBodySetups[BodyIndex];
			Obj->SetBoolField(TEXT("valid"), true);
			Obj->SetStringField(TEXT("name"), BodySetup->GetName());
			Obj->SetStringField(TEXT("bone_name"), BodySetup->BoneName.ToString());
			Obj->SetStringField(TEXT("physics_type"), PhysicsTypeToString(BodySetup->PhysicsType));
			Obj->SetNumberField(TEXT("primitive_count"), GetPrimitiveCount(BodySetup->AggGeom));
			return Obj;
		}

		static TSharedPtr<FJsonObject> MakeConstraintSummaryJson(const UPhysicsAsset& PhysicsAsset, const int32 ConstraintIndex)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetNumberField(TEXT("index"), ConstraintIndex);
			if (!PhysicsAsset.ConstraintSetup.IsValidIndex(ConstraintIndex) || !PhysicsAsset.ConstraintSetup[ConstraintIndex])
			{
				Obj->SetBoolField(TEXT("valid"), false);
				return Obj;
			}
			const UPhysicsConstraintTemplate* Constraint = PhysicsAsset.ConstraintSetup[ConstraintIndex];
			Obj->SetBoolField(TEXT("valid"), true);
			Obj->SetStringField(TEXT("name"), Constraint->GetName());
			Obj->SetStringField(TEXT("joint_name"), Constraint->DefaultInstance.JointName.ToString());
			Obj->SetStringField(TEXT("child_bone"), Constraint->DefaultInstance.ConstraintBone1.ToString());
			Obj->SetStringField(TEXT("parent_bone"), Constraint->DefaultInstance.ConstraintBone2.ToString());
			return Obj;
		}

		static bool TryResolveBodyIndex(const UPhysicsAsset& PhysicsAsset, const TSharedPtr<FJsonObject>& Obj, int32& OutBodyIndex, FString& OutError)
		{
			double IndexValue = 0.0;
			if (TryGetNumberField(Obj, TEXT("body_index"), IndexValue) || TryGetNumberField(Obj, TEXT("index"), IndexValue))
			{
				const int32 BodyIndex = static_cast<int32>(IndexValue);
				if (!PhysicsAsset.SkeletalBodySetups.IsValidIndex(BodyIndex) || !PhysicsAsset.SkeletalBodySetups[BodyIndex])
				{
					OutError = TEXT("physics_asset_body_index_invalid");
					return false;
				}
				OutBodyIndex = BodyIndex;
				return true;
			}

			FString BodyName;
			if (!TryGetStringField(Obj, TEXT("body"), BodyName)
				&& !TryGetStringField(Obj, TEXT("body_name"), BodyName)
				&& !TryGetStringField(Obj, TEXT("bone"), BodyName)
				&& !TryGetStringField(Obj, TEXT("bone_name"), BodyName))
			{
				OutError = TEXT("missing_body_name");
				return false;
			}

			const FName RequestedName(*BodyName);
			int32 BodyIndex = PhysicsAsset.FindBodyIndex(RequestedName);
			if (BodyIndex == INDEX_NONE)
			{
				for (int32 Index = 0; Index < PhysicsAsset.SkeletalBodySetups.Num(); ++Index)
				{
					const USkeletalBodySetup* BodySetup = PhysicsAsset.SkeletalBodySetups[Index];
					if (BodySetup
						&& (BodySetup->GetFName() == RequestedName
							|| BodySetup->GetName().Equals(BodyName, ESearchCase::IgnoreCase)
							|| BodySetup->BoneName.ToString().Equals(BodyName, ESearchCase::IgnoreCase)))
					{
						BodyIndex = Index;
						break;
					}
				}
			}
			if (BodyIndex == INDEX_NONE || !PhysicsAsset.SkeletalBodySetups.IsValidIndex(BodyIndex) || !PhysicsAsset.SkeletalBodySetups[BodyIndex])
			{
				OutError = TEXT("physics_asset_body_not_found");
				return false;
			}
			OutBodyIndex = BodyIndex;
			return true;
		}

		static bool TryResolveConstraintIndex(const UPhysicsAsset& PhysicsAsset, const TSharedPtr<FJsonObject>& Obj, int32& OutConstraintIndex, FString& OutError)
		{
			double IndexValue = 0.0;
			if (TryGetNumberField(Obj, TEXT("constraint_index"), IndexValue) || TryGetNumberField(Obj, TEXT("index"), IndexValue))
			{
				const int32 ConstraintIndex = static_cast<int32>(IndexValue);
				if (!PhysicsAsset.ConstraintSetup.IsValidIndex(ConstraintIndex) || !PhysicsAsset.ConstraintSetup[ConstraintIndex])
				{
					OutError = TEXT("physics_asset_constraint_index_invalid");
					return false;
				}
				OutConstraintIndex = ConstraintIndex;
				return true;
			}

			FString ConstraintName;
			if (TryGetStringField(Obj, TEXT("constraint"), ConstraintName)
				|| TryGetStringField(Obj, TEXT("constraint_name"), ConstraintName)
				|| TryGetStringField(Obj, TEXT("joint"), ConstraintName)
				|| TryGetStringField(Obj, TEXT("joint_name"), ConstraintName))
			{
				const FName RequestedName(*ConstraintName);
				int32 ConstraintIndex = PhysicsAsset.FindConstraintIndex(RequestedName);
				if (ConstraintIndex == INDEX_NONE)
				{
					for (int32 Index = 0; Index < PhysicsAsset.ConstraintSetup.Num(); ++Index)
					{
						const UPhysicsConstraintTemplate* Constraint = PhysicsAsset.ConstraintSetup[Index];
						if (Constraint
							&& (Constraint->GetFName() == RequestedName
								|| Constraint->GetName().Equals(ConstraintName, ESearchCase::IgnoreCase)
								|| Constraint->DefaultInstance.JointName == RequestedName
								|| Constraint->DefaultInstance.JointName.ToString().Equals(ConstraintName, ESearchCase::IgnoreCase)))
						{
							ConstraintIndex = Index;
							break;
						}
					}
				}
				if (ConstraintIndex == INDEX_NONE || !PhysicsAsset.ConstraintSetup.IsValidIndex(ConstraintIndex) || !PhysicsAsset.ConstraintSetup[ConstraintIndex])
				{
					OutError = TEXT("physics_asset_constraint_not_found");
					return false;
				}
				OutConstraintIndex = ConstraintIndex;
				return true;
			}

			FString ChildBone;
			FString ParentBone;
			const bool bHasChild = TryGetStringField(Obj, TEXT("child_bone"), ChildBone) || TryGetStringField(Obj, TEXT("bone1"), ChildBone);
			const bool bHasParent = TryGetStringField(Obj, TEXT("parent_bone"), ParentBone) || TryGetStringField(Obj, TEXT("bone2"), ParentBone);
			if (bHasChild && bHasParent)
			{
				const int32 ConstraintIndex = PhysicsAsset.FindConstraintIndex(FName(*ChildBone), FName(*ParentBone));
				if (ConstraintIndex != INDEX_NONE && PhysicsAsset.ConstraintSetup.IsValidIndex(ConstraintIndex) && PhysicsAsset.ConstraintSetup[ConstraintIndex])
				{
					OutConstraintIndex = ConstraintIndex;
					return true;
				}
			}

			OutError = TEXT("missing_constraint_name");
			return false;
		}

		static bool GetBodyBoneTransform(const UPhysicsAsset& PhysicsAsset, USkinnedMeshComponent& Component, const int32 BodyIndex, FTransform& OutBoneTM, FString& OutError)
		{
			if (!PhysicsAsset.SkeletalBodySetups.IsValidIndex(BodyIndex) || !PhysicsAsset.SkeletalBodySetups[BodyIndex])
			{
				OutError = TEXT("physics_asset_body_index_invalid");
				return false;
			}
			const FName BoneName = PhysicsAsset.SkeletalBodySetups[BodyIndex]->BoneName;
			const int32 BoneIndex = Component.GetBoneIndex(BoneName);
			if (BoneIndex == INDEX_NONE)
			{
				OutError = FString::Printf(TEXT("physics_asset_body_bone_not_found:%s"), *BoneName.ToString());
				return false;
			}
			OutBoneTM = Component.GetBoneTransform(BoneIndex);
			return true;
		}

		static bool ComputeBodyBounds(const UPhysicsAsset& PhysicsAsset, USkinnedMeshComponent& Component, const int32 BodyIndex, FBox& OutBounds, FString& OutError)
		{
			OutBounds = FBox(ForceInit);
			if (!PhysicsAsset.SkeletalBodySetups.IsValidIndex(BodyIndex) || !PhysicsAsset.SkeletalBodySetups[BodyIndex])
			{
				OutError = TEXT("physics_asset_body_index_invalid");
				return false;
			}

			FTransform BoneTM;
			if (!GetBodyBoneTransform(PhysicsAsset, Component, BodyIndex, BoneTM, OutError))
			{
				return false;
			}
			const USkeletalBodySetup* BodySetup = PhysicsAsset.SkeletalBodySetups[BodyIndex];
			const FKAggregateGeom& AggGeom = BodySetup->AggGeom;
			const float Scale = static_cast<float>(BoneTM.GetScale3D().GetAbsMax());
			BoneTM.RemoveScaling();

			for (const FKSphereElem& Elem : AggGeom.SphereElems)
			{
				OutBounds += Elem.CalcAABB(BoneTM, Scale);
			}
			for (const FKBoxElem& Elem : AggGeom.BoxElems)
			{
				OutBounds += Elem.CalcAABB(BoneTM, Scale);
			}
			for (const FKSphylElem& Elem : AggGeom.SphylElems)
			{
				OutBounds += Elem.CalcAABB(BoneTM, Scale);
			}
			for (const FKConvexElem& Elem : AggGeom.ConvexElems)
			{
				OutBounds += Elem.CalcAABB(BoneTM, BoneTM.GetScale3D());
			}
			for (const FKTaperedCapsuleElem& Elem : AggGeom.TaperedCapsuleElems)
			{
				OutBounds += Elem.CalcAABB(BoneTM, Scale);
			}
			for (const FKLevelSetElem& Elem : AggGeom.LevelSetElems)
			{
				OutBounds += Elem.CalcAABB(BoneTM, BoneTM.GetScale3D());
			}
			for (const FKSkinnedLevelSetElem& Elem : AggGeom.SkinnedLevelSetElems)
			{
				OutBounds += Elem.CalcAABB(BoneTM, BoneTM.GetScale3D());
			}
			for (const FKMLLevelSetElem& Elem : AggGeom.MLLevelSetElems)
			{
				OutBounds += Elem.CalcAABB(BoneTM, BoneTM.GetScale3D());
			}
			for (const FKSkinnedTriangleMeshElem& Elem : AggGeom.SkinnedTriangleMeshElems)
			{
				OutBounds += Elem.CalcAABB(BoneTM, BoneTM.GetScale3D());
			}

			if (!OutBounds.IsValid)
			{
				OutError = TEXT("physics_asset_body_has_no_bounds");
				return false;
			}
			return true;
		}

		static bool ComputeAllBodyBounds(const UPhysicsAsset& PhysicsAsset, USkinnedMeshComponent& Component, FBox& OutBounds)
		{
			OutBounds = FBox(ForceInit);
			for (int32 BodyIndex = 0; BodyIndex < PhysicsAsset.SkeletalBodySetups.Num(); ++BodyIndex)
			{
				FString IgnoredError;
				FBox BodyBounds(ForceInit);
				if (ComputeBodyBounds(PhysicsAsset, Component, BodyIndex, BodyBounds, IgnoredError))
				{
					OutBounds += BodyBounds;
				}
			}
			return OutBounds.IsValid != 0;
		}

		static bool GetConstraintWorldTransform(
			const UPhysicsAsset& PhysicsAsset,
			USkinnedMeshComponent& Component,
			const int32 ConstraintIndex,
			const EConstraintFrame::Type Frame,
			FTransform& OutTransform,
			FString& OutError)
		{
			if (!PhysicsAsset.ConstraintSetup.IsValidIndex(ConstraintIndex) || !PhysicsAsset.ConstraintSetup[ConstraintIndex])
			{
				OutError = TEXT("physics_asset_constraint_index_invalid");
				return false;
			}
			const UPhysicsConstraintTemplate* Constraint = PhysicsAsset.ConstraintSetup[ConstraintIndex];
			const FName BoneName = Frame == EConstraintFrame::Frame1
				? Constraint->DefaultInstance.ConstraintBone1
				: Constraint->DefaultInstance.ConstraintBone2;
			const int32 BoneIndex = Component.GetBoneIndex(BoneName);
			if (BoneIndex == INDEX_NONE)
			{
				OutError = FString::Printf(TEXT("physics_asset_constraint_bone_not_found:%s"), *BoneName.ToString());
				return false;
			}
			FTransform LocalFrame = Constraint->DefaultInstance.GetRefFrame(Frame);
			OutTransform = LocalFrame * Component.GetBoneTransform(BoneIndex);
			return true;
		}

		static TSharedPtr<FJsonObject> MakePhysicsVisualOptionsJson(FAnimationViewportClient& Client, UDebugSkelMeshComponent* Component, UPhysicsAsset* PhysicsAsset, const FPhysicsAssetRenderSettings* Settings)
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
			Overlays.Add(TEXT("collision"), Settings && Settings->CollisionViewMode != EPhysicsAssetEditorCollisionViewMode::None);
			Overlays.Add(TEXT("bodies"), Settings && PhysicsAsset ? Settings->HiddenBodies.Num() < PhysicsAsset->SkeletalBodySetups.Num() : false);
			Overlays.Add(TEXT("constraints"), Settings && PhysicsAsset ? (Settings->ConstraintViewMode != EPhysicsAssetEditorConstraintViewMode::None && Settings->HiddenConstraints.Num() < PhysicsAsset->ConstraintSetup.Num()) : false);
			Overlays.Add(TEXT("center_of_mass"), Settings && Settings->CenterOfMassViewMode != EPhysicsAssetEditorCenterOfMassViewMode::None);
			Obj->SetObjectField(TEXT("overlays"), MakeBoolOptionsJson(Overlays));
			Obj->SetStringField(TEXT("bone_draw_mode"), BoneDrawModeToString(Client.GetBoneDrawMode()));
			Obj->SetStringField(TEXT("local_axes_mode"), LocalAxesModeToString(Client.GetLocalAxesMode()));
			Obj->SetBoolField(TEXT("realtime"), Client.IsRealtime());

			TSharedPtr<FJsonObject> PhysicsObj = MakeShared<FJsonObject>();
			PhysicsObj->SetStringField(TEXT("collision_view_mode"), Settings ? CollisionViewModeToString(Settings->CollisionViewMode) : TEXT("unknown"));
			PhysicsObj->SetStringField(TEXT("constraint_view_mode"), Settings ? ConstraintViewModeToString(Settings->ConstraintViewMode) : TEXT("unknown"));
			PhysicsObj->SetStringField(TEXT("center_of_mass_view_mode"), Settings ? CenterOfMassViewModeToString(Settings->CenterOfMassViewMode) : TEXT("unknown"));
			PhysicsObj->SetNumberField(TEXT("body_count"), PhysicsAsset ? PhysicsAsset->SkeletalBodySetups.Num() : 0);
			PhysicsObj->SetNumberField(TEXT("constraint_count"), PhysicsAsset ? PhysicsAsset->ConstraintSetup.Num() : 0);
			PhysicsObj->SetNumberField(TEXT("hidden_body_count"), Settings ? Settings->HiddenBodies.Num() : 0);
			PhysicsObj->SetNumberField(TEXT("hidden_constraint_count"), Settings ? Settings->HiddenConstraints.Num() : 0);
			PhysicsObj->SetNumberField(TEXT("constraint_draw_size"), Settings ? Settings->ConstraintDrawSize : 0.0f);
			PhysicsObj->SetNumberField(TEXT("collision_opacity"), Settings ? Settings->PhysicsBlend : 0.0f);
			PhysicsObj->SetBoolField(TEXT("hide_kinematic_bodies"), Settings ? Settings->bHideKinematicBodies : false);
			PhysicsObj->SetBoolField(TEXT("hide_simulated_bodies"), Settings ? Settings->bHideSimulatedBodies : false);
			PhysicsObj->SetBoolField(TEXT("body_mass"), Settings ? !Settings->bHideBodyMass : false);
			PhysicsObj->SetBoolField(TEXT("constraints_as_points"), Settings ? Settings->bShowConstraintsAsPoints : false);
			PhysicsObj->SetBoolField(TEXT("render_only_selected_constraints"), Settings ? Settings->bRenderOnlySelectedConstraints : false);
			PhysicsObj->SetBoolField(TEXT("draw_violated_limits"), Settings ? Settings->bDrawViolatedLimits : false);
			Obj->SetObjectField(TEXT("physics_asset"), PhysicsObj);
			return Obj;
		}

		static TSharedPtr<FJsonObject> MakePhysicsPreviewStateJson(const FPhysicsAssetRuntime& Runtime)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("context"), TEXT("physics_asset"));
			Obj->SetStringField(TEXT("asset_path"), Runtime.PhysicsAsset ? Runtime.PhysicsAsset->GetPathName() : FString());
			USkeletalMesh* PreviewMesh = Runtime.PhysicsAsset ? Runtime.PhysicsAsset->GetPreviewMesh() : nullptr;
			Obj->SetStringField(TEXT("preview_mesh_asset_path"), PreviewMesh ? PreviewMesh->GetPathName() : FString());
			Obj->SetNumberField(TEXT("body_count"), Runtime.PhysicsAsset ? Runtime.PhysicsAsset->SkeletalBodySetups.Num() : 0);
			Obj->SetNumberField(TEXT("constraint_count"), Runtime.PhysicsAsset ? Runtime.PhysicsAsset->ConstraintSetup.Num() : 0);

			TArray<TSharedPtr<FJsonValue>> BodyValues;
			if (Runtime.PhysicsAsset)
			{
				for (int32 BodyIndex = 0; BodyIndex < Runtime.PhysicsAsset->SkeletalBodySetups.Num(); ++BodyIndex)
				{
					BodyValues.Add(MakeShared<FJsonValueObject>(MakeBodySummaryJson(*Runtime.PhysicsAsset, BodyIndex)));
				}
			}
			Obj->SetArrayField(TEXT("bodies"), BodyValues);

			TArray<TSharedPtr<FJsonValue>> ConstraintValues;
			if (Runtime.PhysicsAsset)
			{
				for (int32 ConstraintIndex = 0; ConstraintIndex < Runtime.PhysicsAsset->ConstraintSetup.Num(); ++ConstraintIndex)
				{
					ConstraintValues.Add(MakeShared<FJsonValueObject>(MakeConstraintSummaryJson(*Runtime.PhysicsAsset, ConstraintIndex)));
				}
			}
			Obj->SetArrayField(TEXT("constraints"), ConstraintValues);
			return Obj;
		}

		static bool IsReadOnlyPreviewStateKey(const FString& Key)
		{
			return Key == TEXT("context")
				|| Key == TEXT("asset_path")
				|| Key == TEXT("preview_mesh_asset_path")
				|| Key == TEXT("body_count")
				|| Key == TEXT("constraint_count")
				|| Key == TEXT("bodies")
				|| Key == TEXT("constraints");
		}

		class FPhysicsAssetViewportAdapter final : public IViewportAdapter
		{
		public:
			virtual FString GetName() const override
			{
				return TEXT("physics_asset");
			}

			virtual bool CanResolve(const FResolveRequest& Request) const override
			{
				if (Request.Target.Scope != TEXT("asset_editor"))
				{
					return false;
				}
				return Request.Target.AssetType.IsEmpty() || IsPhysicsAssetType(Request.Target.AssetType);
			}

			virtual bool Resolve(const FResolveRequest& Request, FResolvedViewport& OutViewport, FString& OutError) const override
			{
				if (Request.Target.AssetPath.IsEmpty())
				{
					OutError = TEXT("missing_asset_path");
					return false;
				}
				UPhysicsAsset* PhysicsAsset = ResolvePhysicsAsset(Request.Target.AssetPath, Request.Target.AssetType, OutError);
				if (!PhysicsAsset)
				{
					return false;
				}

				IAssetEditorInstance* EditorInstance = nullptr;
				if (!OpenEditorIfNeeded(PhysicsAsset, Request.Target.bOpenEditorIfNeeded, EditorInstance, OutError))
				{
					return false;
				}
				if (EditorInstance->GetEditorName() != FName(TEXT("PhysicsAssetEditor")))
				{
					OutError = FString::Printf(TEXT("unsupported_physics_asset_editor:%s"), *EditorInstance->GetEditorName().ToString());
					return false;
				}
				FAssetEditorToolkit* Toolkit = static_cast<FAssetEditorToolkit*>(EditorInstance);
				if (!Toolkit || !ToolkitEditsAsset(*Toolkit, PhysicsAsset))
				{
					OutError = TEXT("physics_asset_editor_asset_mismatch");
					return false;
				}

				IPhysicsAssetEditor* PhysicsAssetEditor = static_cast<IPhysicsAssetEditor*>(Toolkit);
				TSharedRef<IPersonaPreviewScene> PreviewScene = PhysicsAssetEditor->GetPersonaToolkit()->GetPreviewScene();
				FAnimationViewportClient* Client = FindPersonaViewportClient(PreviewScene);
				if (!Client)
				{
					ResolvePreviewViewportWidget(*Toolkit);
					Client = FindPersonaViewportClient(PreviewScene);
				}
				if (!Client)
				{
					OutError = TEXT("physics_asset_preview_viewport_not_found");
					return false;
				}

				OutViewport.Target = Request.Target;
				OutViewport.Target.Scope = TEXT("asset_editor");
				OutViewport.Target.AssetPath = PhysicsAsset->GetPathName();
				OutViewport.Target.AssetType = TEXT("physics_asset");
				OutViewport.Target.ViewportRole = NormalizeToken(Request.Target.ViewportRole.IsEmpty() ? TEXT("primary") : Request.Target.ViewportRole);
				if (OutViewport.Target.ViewportRole.IsEmpty())
				{
					OutViewport.Target.ViewportRole = TEXT("primary");
				}
				OutViewport.Target.ViewportId = FString::Printf(TEXT("asset:%s#physics_asset_%s"), *PhysicsAsset->GetPathName(), *OutViewport.Target.ViewportRole);
				OutViewport.AdapterName = GetName();
				OutViewport.ClientClass = TEXT("FAnimationViewportClient");
				OutViewport.ViewportClient = Client;
				OutViewport.Asset = PhysicsAsset;
				return true;
			}

			virtual bool ReadState(const FResolvedViewport& Viewport, FViewportState& OutState, FString& OutError) const override
			{
				FPhysicsAssetRuntime Runtime;
				if (!ResolveRuntime(Viewport, Runtime, OutError))
				{
					return false;
				}

				OutState.Target = Viewport.Target;
				OutState.ClientClass = TEXT("FAnimationViewportClient");
				OutState.Adapter = GetName();
				OutState.Camera.Location = Runtime.Client->GetViewLocation();
				OutState.Camera.Rotation = Runtime.Client->GetViewRotation();
				OutState.Camera.Fov = Runtime.Client->ViewFOV;
				OutState.Camera.Space = PhysicsAssetCameraSpace;
				OutState.Camera.Projection = Runtime.Client->ViewportType == LVT_Perspective ? TEXT("perspective") : TEXT("orthographic");
				OutState.Camera.bValid = true;
				OutState.Capabilities.bCamera = true;
				OutState.Capabilities.bVisualOptions = true;
				OutState.Capabilities.bPreviewState = true;

				OutState.Capabilities.FocusModes = { TEXT("preview_component_bounds"), TEXT("asset_bounds"), TEXT("bounds"), TEXT("selection"), TEXT("body"), TEXT("constraint"), TEXT("custom_bounds"), TEXT("custom_point") };

				OutState.Capabilities.VisualOptionKeys = { TEXT("view_mode"), TEXT("grid"), TEXT("bounds"), TEXT("bones"), TEXT("bone_draw_mode"), TEXT("local_axes"), TEXT("local_axes_mode"), TEXT("sockets"), TEXT("mesh_sockets"), TEXT("skeleton_sockets"), TEXT("normals"), TEXT("tangents"), TEXT("binormals"), TEXT("collision"), TEXT("collision_view_mode"), TEXT("bodies"), TEXT("constraints"), TEXT("constraint_view_mode"), TEXT("center_of_mass"), TEXT("center_of_mass_view_mode"), TEXT("body_mass"), TEXT("constraints_as_points"), TEXT("constraint_draw_size"), TEXT("realtime") };
				OutState.Capabilities.PreviewStateKeys = { TEXT("asset_path"), TEXT("preview_mesh_asset_path"), TEXT("body_count"), TEXT("constraint_count"), TEXT("bodies"), TEXT("constraints") };
				OutState.VisualOptions = MakePhysicsVisualOptionsJson(*Runtime.Client, Runtime.PreviewComponent, Runtime.PhysicsAsset, Runtime.RenderSettings);
				OutState.PreviewState = MakePhysicsPreviewStateJson(Runtime);
				return true;
			}

			virtual bool WriteCamera(const FResolvedViewport& Viewport, const FViewportCamera& Camera, FString& OutError) const override
			{
				FPhysicsAssetRuntime Runtime;
				if (!ResolveRuntime(Viewport, Runtime, OutError))
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
				FPhysicsAssetRuntime Runtime;
				if (!ResolveRuntime(Viewport, Runtime, OutError))
				{
					return false;
				}
				if (!Runtime.PreviewComponent)
				{
					OutError = TEXT("missing_physics_asset_preview_component");
					return false;
				}

				FString Mode = TEXT("asset_bounds");
				TryGetStringField(FocusObj, TEXT("mode"), Mode);
				Mode = NormalizeToken(Mode);

				FBox FocusBox(ForceInit);
				FVector FocusPoint = FVector::ZeroVector;
				bool bUsePoint = false;
				TSharedPtr<FJsonObject> BodyData;
				TSharedPtr<FJsonObject> ConstraintData;

				if (Mode == TEXT("preview_component_bounds"))
				{
					FocusBox = MakeComponentBoundsBox(Runtime.PreviewComponent);
				}
				else if (Mode == TEXT("asset_bounds") || Mode == TEXT("bounds"))
				{
					if (!ComputeAllBodyBounds(*Runtime.PhysicsAsset, *Runtime.PreviewComponent, FocusBox))
					{
						FocusBox = MakeComponentBoundsBox(Runtime.PreviewComponent);
					}
				}
				else if (Mode == TEXT("selection"))
				{
					const FSphere Target = Runtime.Client->GetCameraTarget();
					if (Target.W > 0.0f)
					{
						FocusBox = FBox::BuildAABB(Target.Center, FVector(FMath::Max(Target.W, 1.0f)));
					}
					else if (!ComputeAllBodyBounds(*Runtime.PhysicsAsset, *Runtime.PreviewComponent, FocusBox))
					{
						FocusBox = MakeComponentBoundsBox(Runtime.PreviewComponent);
					}
				}
				else if (Mode == TEXT("body"))
				{
					int32 BodyIndex = INDEX_NONE;
					if (!TryResolveBodyIndex(*Runtime.PhysicsAsset, FocusObj, BodyIndex, OutError))
					{
						return false;
					}
					if (!ComputeBodyBounds(*Runtime.PhysicsAsset, *Runtime.PreviewComponent, BodyIndex, FocusBox, OutError))
					{
						return false;
					}
					BodyData = MakeBodySummaryJson(*Runtime.PhysicsAsset, BodyIndex);
				}
				else if (Mode == TEXT("constraint"))
				{
					int32 ConstraintIndex = INDEX_NONE;
					if (!TryResolveConstraintIndex(*Runtime.PhysicsAsset, FocusObj, ConstraintIndex, OutError))
					{
						return false;
					}
					FTransform ChildFrame;
					FTransform ParentFrame;
					if (!GetConstraintWorldTransform(*Runtime.PhysicsAsset, *Runtime.PreviewComponent, ConstraintIndex, EConstraintFrame::Frame1, ChildFrame, OutError)
						|| !GetConstraintWorldTransform(*Runtime.PhysicsAsset, *Runtime.PreviewComponent, ConstraintIndex, EConstraintFrame::Frame2, ParentFrame, OutError))
					{
						return false;
					}
					double Radius = 25.0;
					TryGetNumberField(FocusObj, TEXT("radius"), Radius);
					const FVector Extent(FMath::Max(static_cast<float>(Radius), 1.0f));
					FocusBox = FBox(ForceInit);
					FocusBox += FBox::BuildAABB(ChildFrame.GetLocation(), Extent);
					FocusBox += FBox::BuildAABB(ParentFrame.GetLocation(), Extent);
					ConstraintData = MakeConstraintSummaryJson(*Runtime.PhysicsAsset, ConstraintIndex);
					ConstraintData->SetObjectField(TEXT("child_frame_location"), MakeVectorJson(ChildFrame.GetLocation()));
					ConstraintData->SetObjectField(TEXT("parent_frame_location"), MakeVectorJson(ParentFrame.GetLocation()));
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
				OutData->SetStringField(TEXT("bounds_space"), PhysicsAssetCameraSpace);
				OutData->SetBoolField(TEXT("instant"), bInstant);
				OutData->SetObjectField(TEXT("bounds"), MakeBoundsJson(FocusBox));
				if (BodyData.IsValid())
				{
					OutData->SetObjectField(TEXT("body"), BodyData);
				}
				if (ConstraintData.IsValid())
				{
					OutData->SetObjectField(TEXT("constraint"), ConstraintData);
				}
				if (Mode == TEXT("selection"))
				{
					OutData->SetStringField(TEXT("selection_source"), TEXT("animation_viewport_client_camera_target"));
				}
				return true;
			}

			virtual bool ApplyVisualOptions(const FResolvedViewport& Viewport, const TSharedPtr<FJsonObject>& Options, TSharedPtr<FJsonObject>& OutData, FString& OutError) const override
			{
				FPhysicsAssetRuntime Runtime;
				if (!ResolveRuntime(Viewport, Runtime, OutError))
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
				if (TryReadStringOption(Options, TEXT("bone_draw_mode"), BoneDrawModeText))
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
				if (TryReadStringOption(Options, TEXT("local_axes_mode"), LocalAxesModeText))
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

				if (Runtime.PreviewComponent)
				{
					bool bSockets = Runtime.PreviewComponent->bDrawSockets != 0;
					if (TryReadBoolOption(Options, TEXT("sockets"), bSockets))
					{
						Runtime.PreviewComponent->bDrawSockets = bSockets;
						if (bSockets && !Runtime.PreviewComponent->bMeshSocketsVisible && !Runtime.PreviewComponent->bSkeletonSocketsVisible)
						{
							Runtime.PreviewComponent->bMeshSocketsVisible = true;
							Runtime.PreviewComponent->bSkeletonSocketsVisible = true;
						}
						Runtime.PreviewComponent->MarkRenderStateDirty();
					}

					bool bMeshSockets = Runtime.PreviewComponent->bMeshSocketsVisible != 0;
					if (TryReadBoolOption(Options, TEXT("mesh_sockets"), bMeshSockets))
					{
						Runtime.PreviewComponent->bMeshSocketsVisible = bMeshSockets;
						if (bMeshSockets)
						{
							Runtime.PreviewComponent->bDrawSockets = true;
						}
						Runtime.PreviewComponent->MarkRenderStateDirty();
					}

					bool bSkeletonSockets = Runtime.PreviewComponent->bSkeletonSocketsVisible != 0;
					if (TryReadBoolOption(Options, TEXT("skeleton_sockets"), bSkeletonSockets))
					{
						Runtime.PreviewComponent->bSkeletonSocketsVisible = bSkeletonSockets;
						if (bSkeletonSockets)
						{
							Runtime.PreviewComponent->bDrawSockets = true;
						}
						Runtime.PreviewComponent->MarkRenderStateDirty();
					}
				}

				ApplyToggleBoolOption(*Runtime.Client, Options, TEXT("normals"), &FAnimationViewportClient::IsSetShowNormalsChecked, &FAnimationViewportClient::ToggleShowNormals);
				ApplyToggleBoolOption(*Runtime.Client, Options, TEXT("tangents"), &FAnimationViewportClient::IsSetShowTangentsChecked, &FAnimationViewportClient::ToggleShowTangents);
				ApplyToggleBoolOption(*Runtime.Client, Options, TEXT("binormals"), &FAnimationViewportClient::IsSetShowBinormalsChecked, &FAnimationViewportClient::ToggleShowBinormals);

				if (Runtime.RenderSettings && Runtime.PhysicsAsset)
				{
					bool bCollision = Runtime.RenderSettings->CollisionViewMode != EPhysicsAssetEditorCollisionViewMode::None;
					if (TryReadBoolOption(Options, TEXT("collision"), bCollision))
					{
						Runtime.RenderSettings->CollisionViewMode = bCollision
							? (Runtime.RenderSettings->CollisionViewMode == EPhysicsAssetEditorCollisionViewMode::None ? EPhysicsAssetEditorCollisionViewMode::SolidWireframe : Runtime.RenderSettings->CollisionViewMode)
							: EPhysicsAssetEditorCollisionViewMode::None;
					}
					FString CollisionModeText;
					if (TryReadStringOption(Options, TEXT("collision_view_mode"), CollisionModeText))
					{
						EPhysicsAssetEditorCollisionViewMode CollisionMode = EPhysicsAssetEditorCollisionViewMode::SolidWireframe;
						if (!TryParseCollisionViewMode(CollisionModeText, CollisionMode))
						{
							Unsupported.Add(TEXT("collision_view_mode"));
						}
						else
						{
							Runtime.RenderSettings->CollisionViewMode = CollisionMode;
						}
					}

					bool bBodies = Runtime.RenderSettings->HiddenBodies.Num() < Runtime.PhysicsAsset->SkeletalBodySetups.Num();
					if (TryReadBoolOption(Options, TEXT("bodies"), bBodies))
					{
						if (bBodies)
						{
							Runtime.RenderSettings->ShowAllBodies();
							if (Runtime.RenderSettings->CollisionViewMode == EPhysicsAssetEditorCollisionViewMode::None)
							{
								Runtime.RenderSettings->CollisionViewMode = EPhysicsAssetEditorCollisionViewMode::SolidWireframe;
							}
						}
						else
						{
							Runtime.RenderSettings->HideAllBodies(Runtime.PhysicsAsset);
						}
					}

					bool bConstraints = Runtime.RenderSettings->ConstraintViewMode != EPhysicsAssetEditorConstraintViewMode::None
						&& Runtime.RenderSettings->HiddenConstraints.Num() < Runtime.PhysicsAsset->ConstraintSetup.Num();
					if (TryReadBoolOption(Options, TEXT("constraints"), bConstraints))
					{
						if (bConstraints)
						{
							Runtime.RenderSettings->ShowAllConstraints();
							if (Runtime.RenderSettings->ConstraintViewMode == EPhysicsAssetEditorConstraintViewMode::None)
							{
								Runtime.RenderSettings->ConstraintViewMode = EPhysicsAssetEditorConstraintViewMode::AllLimits;
							}
						}
						else
						{
							Runtime.RenderSettings->HideAllConstraints(Runtime.PhysicsAsset);
							Runtime.RenderSettings->ConstraintViewMode = EPhysicsAssetEditorConstraintViewMode::None;
						}
					}
					FString ConstraintModeText;
					if (TryReadStringOption(Options, TEXT("constraint_view_mode"), ConstraintModeText))
					{
						EPhysicsAssetEditorConstraintViewMode ConstraintMode = EPhysicsAssetEditorConstraintViewMode::AllLimits;
						if (!TryParseConstraintViewMode(ConstraintModeText, ConstraintMode))
						{
							Unsupported.Add(TEXT("constraint_view_mode"));
						}
						else
						{
							Runtime.RenderSettings->ConstraintViewMode = ConstraintMode;
						}
					}

					bool bCenterOfMass = Runtime.RenderSettings->CenterOfMassViewMode != EPhysicsAssetEditorCenterOfMassViewMode::None;
					if (TryReadBoolOption(Options, TEXT("center_of_mass"), bCenterOfMass) || TryReadBoolOption(Options, TEXT("com"), bCenterOfMass))
					{
						Runtime.RenderSettings->CenterOfMassViewMode = bCenterOfMass ? EPhysicsAssetEditorCenterOfMassViewMode::All : EPhysicsAssetEditorCenterOfMassViewMode::None;
					}
					FString CenterOfMassModeText;
					if (TryReadStringOption(Options, TEXT("center_of_mass_view_mode"), CenterOfMassModeText) || TryReadStringOption(Options, TEXT("com_view_mode"), CenterOfMassModeText))
					{
						EPhysicsAssetEditorCenterOfMassViewMode CenterOfMassMode = EPhysicsAssetEditorCenterOfMassViewMode::All;
						if (!TryParseCenterOfMassViewMode(CenterOfMassModeText, CenterOfMassMode))
						{
							Unsupported.Add(TEXT("center_of_mass_view_mode"));
						}
						else
						{
							Runtime.RenderSettings->CenterOfMassViewMode = CenterOfMassMode;
						}
					}

					bool bBodyMass = !Runtime.RenderSettings->bHideBodyMass;
					if (TryReadBoolOption(Options, TEXT("body_mass"), bBodyMass))
					{
						Runtime.RenderSettings->bHideBodyMass = !bBodyMass;
					}
					bool bConstraintsAsPoints = Runtime.RenderSettings->bShowConstraintsAsPoints;
					if (TryReadBoolOption(Options, TEXT("constraints_as_points"), bConstraintsAsPoints))
					{
						Runtime.RenderSettings->bShowConstraintsAsPoints = bConstraintsAsPoints;
					}
					bool bRenderOnlySelectedConstraints = Runtime.RenderSettings->bRenderOnlySelectedConstraints;
					if (TryReadBoolOption(Options, TEXT("render_only_selected_constraints"), bRenderOnlySelectedConstraints))
					{
						Runtime.RenderSettings->bRenderOnlySelectedConstraints = bRenderOnlySelectedConstraints;
					}
					bool bDrawViolatedLimits = Runtime.RenderSettings->bDrawViolatedLimits;
					if (TryReadBoolOption(Options, TEXT("draw_violated_limits"), bDrawViolatedLimits))
					{
						Runtime.RenderSettings->bDrawViolatedLimits = bDrawViolatedLimits;
					}
					bool bHideKinematicBodies = Runtime.RenderSettings->bHideKinematicBodies;
					if (TryReadBoolOption(Options, TEXT("hide_kinematic_bodies"), bHideKinematicBodies))
					{
						Runtime.RenderSettings->bHideKinematicBodies = bHideKinematicBodies;
					}
					bool bHideSimulatedBodies = Runtime.RenderSettings->bHideSimulatedBodies;
					if (TryReadBoolOption(Options, TEXT("hide_simulated_bodies"), bHideSimulatedBodies))
					{
						Runtime.RenderSettings->bHideSimulatedBodies = bHideSimulatedBodies;
					}
					double NumberValue = 0.0;
					if (TryReadNumberOption(Options, TEXT("constraint_draw_size"), NumberValue))
					{
						Runtime.RenderSettings->ConstraintDrawSize = FMath::Clamp(static_cast<float>(NumberValue), 0.01f, 100.0f);
					}
					if (TryReadNumberOption(Options, TEXT("collision_opacity"), NumberValue))
					{
						Runtime.RenderSettings->PhysicsBlend = FMath::Clamp(static_cast<float>(NumberValue), 0.0f, 1.0f);
					}
				}

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

				OutData = MakePhysicsVisualOptionsJson(*Runtime.Client, Runtime.PreviewComponent, Runtime.PhysicsAsset, Runtime.RenderSettings);
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
				FPhysicsAssetRuntime Runtime;
				if (!ResolveRuntime(Viewport, Runtime, OutError))
				{
					return false;
				}
				OutData = MakePhysicsPreviewStateJson(Runtime);
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

	TSharedRef<IViewportAdapter> MakePhysicsAssetViewportAdapter()
	{
		return MakeShared<FPhysicsAssetViewportAdapter>();
	}
}
