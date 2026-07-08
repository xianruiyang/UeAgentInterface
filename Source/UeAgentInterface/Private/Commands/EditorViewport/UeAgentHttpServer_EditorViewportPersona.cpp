// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commands/EditorViewport/UeAgentHttpServer_EditorViewportInternal.h"

#include "Server/UeAgentHttpServer.h"
#include "UeAgentInterfaceSettings.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimationAsset.h"
#include "Animation/DebugSkelMeshComponent.h"
#include "Animation/Skeleton.h"
#include "AnimationEditorViewportClient.h"
#include "ControlRig.h"
#include "ControlRigBlueprint.h"
#include "Editor.h"
#include "Engine/SkeletalMeshSocket.h"
#include "HAL/FileManager.h"
#include "IAnimationBlueprintEditor.h"
#include "IAnimationEditor.h"
#include "IHasPersonaToolkit.h"
#include "IPersonaPreviewScene.h"
#include "IPersonaToolkit.h"
#include "ISkeletalMeshEditor.h"
#include "ISkeletonEditor.h"
#include "Misc/PackageName.h"
#include "PersonaTabs.h"
#include "Rigs/RigHierarchy.h"
#include "SEditorViewport.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Widgets/Docking/SDockTab.h"

namespace UeAgentEditorViewport
{
	class FPersonaViewportAdapter final : public IViewportAdapter
	{
	public:
		explicit FPersonaViewportAdapter(const FString& InAdapterName = TEXT("persona"), const bool bInControlRigOnly = false)
			: AdapterName(InAdapterName)
			, bControlRigOnly(bInControlRigOnly)
		{
		}

		virtual FString GetName() const override
		{
			return AdapterName;
		}

		virtual bool CanResolve(const FResolveRequest& Request) const override
		{
			if (Request.Target.Scope != TEXT("asset_editor"))
			{
				return false;
			}
			if (bControlRigOnly)
			{
				return Request.Target.AssetType.IsEmpty() || IsControlRigAssetType(Request.Target.AssetType);
			}
			return Request.Target.AssetType.IsEmpty()
				|| (!IsControlRigAssetType(Request.Target.AssetType) && IsSupportedPersonaAssetType(Request.Target.AssetType));
		}

		virtual bool Resolve(const FResolveRequest& Request, FResolvedViewport& OutViewport, FString& OutError) const override
		{
			if (Request.Target.AssetPath.IsEmpty())
			{
				OutError = TEXT("missing_asset_path");
				return false;
			}

			if (bControlRigOnly)
			{
				return ResolveControlRigViewport(Request, OutViewport, OutError);
			}

			FString CanonicalAssetType;
			UObject* Asset = ResolvePersonaAsset(Request.Target.AssetPath, Request.Target.AssetType, CanonicalAssetType, OutError);
			if (!Asset)
			{
				return false;
			}

			IHasPersonaToolkit* PersonaHost = GetPersonaHostFromAsset(Asset, CanonicalAssetType, Request.Target.bOpenEditorIfNeeded, OutError);
			if (!PersonaHost)
			{
				return false;
			}

			const TSharedRef<IPersonaToolkit> PersonaToolkit = PersonaHost->GetPersonaToolkit();
			const TSharedRef<IPersonaPreviewScene> PreviewScene = PersonaToolkit->GetPreviewScene();
			FAnimationViewportClient* Client = FindPersonaViewportClient(PreviewScene);
			if (!Client)
			{
				OutError = TEXT("persona_viewport_not_found");
				return false;
			}

			OutViewport.Target = Request.Target;
			OutViewport.Target.Scope = TEXT("asset_editor");
			OutViewport.Target.AssetPath = Asset->GetPathName();
			OutViewport.Target.AssetType = CanonicalAssetType;
			OutViewport.Target.ViewportRole = TEXT("primary");
			OutViewport.Target.ViewportId = FString::Printf(TEXT("asset:%s#persona_primary"), *Asset->GetPathName());
			OutViewport.AdapterName = GetName();
			OutViewport.ClientClass = TEXT("FAnimationViewportClient");
			OutViewport.ViewportClient = Client;
			OutViewport.Asset = Asset;
			return true;
		}

		virtual bool ReadState(const FResolvedViewport& Viewport, FViewportState& OutState, FString& OutError) const override
		{
			FAnimationViewportClient* Client = AsPersonaClient(Viewport);
			if (!Client)
			{
				OutError = TEXT("invalid_persona_viewport");
				return false;
			}

			const TSharedRef<IPersonaPreviewScene> PreviewScene = Client->GetPreviewScene();
			const TSharedRef<IPersonaToolkit> PersonaToolkit = PreviewScene->GetPersonaToolkit();

			OutState.Target = Viewport.Target;
			OutState.ClientClass = TEXT("FAnimationViewportClient");
			OutState.Adapter = GetName();
			OutState.Camera.Location = Client->GetViewLocation();
			OutState.Camera.Rotation = Client->GetViewRotation();
			OutState.Camera.Fov = Client->ViewFOV;
			OutState.Camera.Space = TEXT("persona_preview_world");
			OutState.Camera.Projection = Client->ViewportType == LVT_Perspective ? TEXT("perspective") : TEXT("orthographic");
			OutState.Camera.bValid = true;
			OutState.Capabilities.bCamera = true;
			OutState.Capabilities.bVisualOptions = true;
			OutState.Capabilities.bPreviewState = true;
			OutState.Capabilities.bScreenshot = true;
			OutState.Capabilities.FocusModes = { TEXT("preview_component_bounds"), TEXT("asset_bounds"), TEXT("bounds"), TEXT("selection"), TEXT("bone"), TEXT("socket"), TEXT("custom_bounds"), TEXT("custom_point") };
			if (bControlRigOnly)
			{
				OutState.Capabilities.FocusModes.Add(TEXT("control"));
				OutState.Capabilities.FocusModes.Add(TEXT("null"));
				OutState.Capabilities.FocusModes.Add(TEXT("space"));
			}
			OutState.Capabilities.ScreenshotBackends = { TEXT("persona_editor_backbuffer") };
			OutState.Capabilities.VisualOptionKeys = { TEXT("view_mode"), TEXT("grid"), TEXT("bounds"), TEXT("bones"), TEXT("bone_draw_mode"), TEXT("local_axes"), TEXT("local_axes_mode"), TEXT("sockets"), TEXT("mesh_sockets"), TEXT("skeleton_sockets"), TEXT("normals"), TEXT("tangents"), TEXT("binormals") };
			OutState.Capabilities.PreviewStateKeys = { TEXT("show_reference_pose") };
			OutState.VisualOptions = MakePersonaVisualOptionsJson(*Client, PreviewScene->GetPreviewMeshComponent());
			OutState.PreviewState = MakePersonaPreviewStateJson(*PersonaToolkit, *PreviewScene);
			return true;
		}

		virtual bool WriteCamera(const FResolvedViewport& Viewport, const FViewportCamera& Camera, FString& OutError) const override
		{
			FAnimationViewportClient* Client = AsPersonaClient(Viewport);
			if (!Client)
			{
				OutError = TEXT("invalid_persona_viewport");
				return false;
			}
			Client->SetCameraFollowMode(EAnimationViewportCameraFollowMode::None);
			Client->SetViewLocation(Camera.Location);
			Client->SetViewRotation(Camera.Rotation);
			Client->ViewFOV = FMath::Clamp(Camera.Fov, 5.0f, 170.0f);
			Client->Invalidate();
			if (Client->Viewport)
			{
				Client->Viewport->Invalidate();
			}
			return true;
		}

		struct FControlRigElementFocusResult
		{
			FRigElementKey Key;
			FString ElementType;
			FString HierarchySource;
			FTransform RigGlobalTransform = FTransform::Identity;
			FTransform PreviewWorldTransform = FTransform::Identity;
		};

		static bool IsControlRigElementFocusMode(const FString& Mode)
		{
			return Mode == TEXT("control") || Mode == TEXT("null") || Mode == TEXT("space");
		}

		static FString ControlRigElementTypeToString(const ERigElementType Type)
		{
			switch (Type)
			{
			case ERigElementType::Control:
				return TEXT("control");
			case ERigElementType::Null:
				return TEXT("null");
			case ERigElementType::Bone:
				return TEXT("bone");
			case ERigElementType::Socket:
				return TEXT("socket");
			default:
				return StaticEnum<ERigElementType>()->GetNameStringByValue(static_cast<int64>(Type)).ToLower();
			}
		}

		static bool TryReadControlRigElementFocusName(const TSharedPtr<FJsonObject>& FocusObj, const FString& Mode, FString& OutName)
		{
			TArray<FString> Fields;
			if (Mode == TEXT("control"))
			{
				Fields = { TEXT("control"), TEXT("control_name"), TEXT("element"), TEXT("element_name"), TEXT("name") };
			}
			else if (Mode == TEXT("space"))
			{
				Fields = { TEXT("space"), TEXT("space_name"), TEXT("null"), TEXT("null_name"), TEXT("element"), TEXT("element_name"), TEXT("name") };
			}
			else
			{
				Fields = { TEXT("null"), TEXT("null_name"), TEXT("space"), TEXT("space_name"), TEXT("element"), TEXT("element_name"), TEXT("name") };
			}

			for (const FString& Field : Fields)
			{
				FString Candidate;
				if (TryGetStringField(FocusObj, Field, Candidate))
				{
					Candidate.TrimStartAndEndInline();
					if (!Candidate.IsEmpty())
					{
						OutName = Candidate;
						return true;
					}
				}
			}

			TSharedPtr<FJsonObject> ElementObj;
			if (TryGetObjectField(FocusObj, TEXT("element"), ElementObj))
			{
				for (const FString& Field : { FString(TEXT("name")), FString(TEXT("element_name")), Mode + TEXT("_name") })
				{
					FString Candidate;
					if (TryGetStringField(ElementObj, Field, Candidate))
					{
						Candidate.TrimStartAndEndInline();
						if (!Candidate.IsEmpty())
						{
							OutName = Candidate;
							return true;
						}
					}
				}
			}
			return false;
		}

		static bool ResolveControlRigElementFocus(
			const FResolvedViewport& Viewport,
			const UDebugSkelMeshComponent& PreviewComponent,
			const TSharedPtr<FJsonObject>& FocusObj,
			const FString& Mode,
			FControlRigElementFocusResult& OutResult,
			FString& OutError)
		{
			UControlRigBlueprint* Blueprint = Cast<UControlRigBlueprint>(Viewport.Asset);
			if (!Blueprint)
			{
				OutError = TEXT("control_rig_asset_not_found");
				return false;
			}

			FString ElementName;
			if (!TryReadControlRigElementFocusName(FocusObj, Mode, ElementName))
			{
				OutError = FString::Printf(TEXT("missing_%s_name"), *Mode);
				return false;
			}

			URigHierarchy* Hierarchy = nullptr;
			FString HierarchySource;
			if (UControlRig* DebuggedRig = Blueprint->GetDebuggedControlRig())
			{
				if (DebuggedRig->GetHierarchy())
				{
					Hierarchy = DebuggedRig->GetHierarchy();
					HierarchySource = TEXT("debugged_control_rig");
				}
			}
			if (!Hierarchy)
			{
				Hierarchy = Blueprint->GetHierarchy();
				HierarchySource = TEXT("blueprint_hierarchy");
			}
			if (!Hierarchy)
			{
				OutError = TEXT("control_rig_hierarchy_not_found");
				return false;
			}

			const ERigElementType KeyType = Mode == TEXT("control") ? ERigElementType::Control : ERigElementType::Null;
			const FRigElementKey Key(FName(*ElementName), KeyType);
			if (KeyType == ERigElementType::Control)
			{
				if (!Hierarchy->Find<FRigControlElement>(Key))
				{
					OutError = TEXT("control_rig_control_not_found");
					return false;
				}
			}
			else if (!Hierarchy->Find<FRigNullElement>(Key))
			{
				OutError = Mode == TEXT("space") ? TEXT("control_rig_space_not_found") : TEXT("control_rig_null_not_found");
				return false;
			}

			OutResult.Key = Key;
			OutResult.ElementType = Mode == TEXT("space") ? TEXT("space") : ControlRigElementTypeToString(KeyType);
			OutResult.HierarchySource = HierarchySource;
			OutResult.RigGlobalTransform = Hierarchy->GetGlobalTransform(Key, false);
			OutResult.PreviewWorldTransform = OutResult.RigGlobalTransform * PreviewComponent.GetComponentTransform();
			return true;
		}

		virtual bool Focus(const FResolvedViewport& Viewport, const TSharedPtr<FJsonObject>& FocusObj, TSharedPtr<FJsonObject>& OutData, FString& OutError) const override
		{
			FAnimationViewportClient* Client = AsPersonaClient(Viewport);
			if (!Client)
			{
				OutError = TEXT("invalid_persona_viewport");
				return false;
			}

			const TSharedRef<IPersonaPreviewScene> PreviewScene = Client->GetPreviewScene();
			UDebugSkelMeshComponent* PreviewComponent = PreviewScene->GetPreviewMeshComponent();
			if (!PreviewComponent)
			{
				OutError = TEXT("missing_persona_preview_component");
				return false;
			}

			FString Mode = TEXT("preview_component_bounds");
			TryGetStringField(FocusObj, TEXT("mode"), Mode);
			Mode = NormalizeToken(Mode);

			FBox FocusBox(ForceInit);
			FVector FocusPoint = FVector::ZeroVector;
			bool bUsePoint = false;
			bool bUsedControlRigElement = false;
			FControlRigElementFocusResult ControlRigElementFocus;
			if (Mode == TEXT("preview_component_bounds") || Mode == TEXT("asset_bounds") || Mode == TEXT("bounds"))
			{
				FocusBox = MakeComponentBoundsBox(PreviewComponent);
			}
			else if (Mode == TEXT("selection"))
			{
				if (!BuildSelectionFocusBox(*PreviewScene, *PreviewComponent, FocusBox))
				{
					FocusBox = MakeComponentBoundsBox(PreviewComponent);
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
				const int32 BoneIndex = PreviewComponent->GetBoneIndex(FName(*BoneName));
				if (BoneIndex == INDEX_NONE)
				{
					OutError = TEXT("persona_bone_not_found");
					return false;
				}
				FocusPoint = PreviewComponent->GetBoneTransform(BoneIndex).GetLocation();
				bUsePoint = true;
			}
			else if (Mode == TEXT("socket"))
			{
				FString SocketName;
				if (!TryGetStringField(FocusObj, TEXT("socket"), SocketName) && !TryGetStringField(FocusObj, TEXT("socket_name"), SocketName))
				{
					OutError = TEXT("missing_socket_name");
					return false;
				}
				const FName SocketFName(*SocketName);
				if (!PreviewComponent->DoesSocketExist(SocketFName))
				{
					OutError = TEXT("persona_socket_not_found");
					return false;
				}
				FocusPoint = PreviewComponent->GetSocketTransform(SocketFName).GetLocation();
				bUsePoint = true;
			}
			else if (bControlRigOnly && IsControlRigElementFocusMode(Mode))
			{
				if (!ResolveControlRigElementFocus(Viewport, *PreviewComponent, FocusObj, Mode, ControlRigElementFocus, OutError))
				{
					return false;
				}
				FocusPoint = ControlRigElementFocus.PreviewWorldTransform.GetLocation();
				bUsePoint = true;
				bUsedControlRigElement = true;
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
			Client->FocusViewportOnSphere(FocusSphere, bInstant);
			Client->SetLookAtLocation(FocusBox.GetCenter(), false);
			Client->Invalidate();
			if (Client->Viewport)
			{
				Client->Viewport->Invalidate();
			}
			PreviewScene->InvalidateViews();

			OutData = MakeShared<FJsonObject>();
			OutData->SetStringField(TEXT("mode"), Mode);
			OutData->SetStringField(TEXT("bounds_space"), TEXT("persona_preview_world"));
			OutData->SetBoolField(TEXT("instant"), bInstant);
			TSharedPtr<FJsonObject> BoundsJson = MakeShared<FJsonObject>();
			BoundsJson->SetObjectField(TEXT("min"), MakeVectorJson(FocusBox.Min));
			BoundsJson->SetObjectField(TEXT("max"), MakeVectorJson(FocusBox.Max));
			BoundsJson->SetObjectField(TEXT("center"), MakeVectorJson(FocusBox.GetCenter()));
			BoundsJson->SetObjectField(TEXT("extent"), MakeVectorJson(FocusBox.GetExtent()));
			OutData->SetObjectField(TEXT("bounds"), BoundsJson);
			if (bUsedControlRigElement)
			{
				OutData->SetStringField(TEXT("element_name"), ControlRigElementFocus.Key.Name.ToString());
				OutData->SetStringField(TEXT("element_type"), ControlRigElementFocus.ElementType);
				OutData->SetStringField(TEXT("rig_element_type"), ControlRigElementTypeToString(ControlRigElementFocus.Key.Type));
				OutData->SetStringField(TEXT("hierarchy_source"), ControlRigElementFocus.HierarchySource);
				OutData->SetStringField(TEXT("rig_global_space"), TEXT("control_rig_global"));
				OutData->SetObjectField(TEXT("rig_global_location"), MakeVectorJson(ControlRigElementFocus.RigGlobalTransform.GetLocation()));
				OutData->SetObjectField(TEXT("preview_world_location"), MakeVectorJson(ControlRigElementFocus.PreviewWorldTransform.GetLocation()));
			}
			return true;
		}

		virtual bool ApplyVisualOptions(const FResolvedViewport& Viewport, const TSharedPtr<FJsonObject>& Options, TSharedPtr<FJsonObject>& OutData, FString& OutError) const override
		{
			FAnimationViewportClient* Client = AsPersonaClient(Viewport);
			if (!Client)
			{
				OutError = TEXT("invalid_persona_viewport");
				return false;
			}
			const TSharedRef<IPersonaPreviewScene> PreviewScene = Client->GetPreviewScene();
			UDebugSkelMeshComponent* PreviewComponent = PreviewScene->GetPreviewMeshComponent();

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

			bool bGrid = Client->IsSetShowGridChecked();
			if (TryReadBoolOption(Options, TEXT("grid"), bGrid) && Client->IsSetShowGridChecked() != bGrid)
			{
				Client->SetShowGrid();
			}

			bool bBounds = Client->IsSetShowBoundsChecked();
			if (TryReadBoolOption(Options, TEXT("bounds"), bBounds))
			{
				Client->SetShowBounds(bBounds);
			}

			bool bBones = Client->GetBoneDrawMode() != EBoneDrawMode::None;
			if (TryReadBoolOption(Options, TEXT("bones"), bBones))
			{
				Client->SetBoneDrawMode(bBones ? EBoneDrawMode::All : EBoneDrawMode::None);
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
					Client->SetBoneDrawMode(BoneDrawMode);
				}
			}

			bool bLocalAxes = Client->GetLocalAxesMode() != ELocalAxesMode::None;
			if (TryReadBoolOption(Options, TEXT("local_axes"), bLocalAxes))
			{
				Client->SetLocalAxesMode(bLocalAxes ? ELocalAxesMode::All : ELocalAxesMode::None);
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
					Client->SetLocalAxesMode(LocalAxesMode);
				}
			}

			if (PreviewComponent)
			{
				bool bSockets = PreviewComponent->bDrawSockets != 0;
				if (TryReadBoolOption(Options, TEXT("sockets"), bSockets))
				{
					PreviewComponent->bDrawSockets = bSockets;
					if (bSockets && !PreviewComponent->bMeshSocketsVisible && !PreviewComponent->bSkeletonSocketsVisible)
					{
						PreviewComponent->bMeshSocketsVisible = true;
						PreviewComponent->bSkeletonSocketsVisible = true;
					}
					PreviewComponent->MarkRenderStateDirty();
				}

				bool bMeshSockets = PreviewComponent->bMeshSocketsVisible != 0;
				if (TryReadBoolOption(Options, TEXT("mesh_sockets"), bMeshSockets))
				{
					PreviewComponent->bMeshSocketsVisible = bMeshSockets;
					if (bMeshSockets)
					{
						PreviewComponent->bDrawSockets = true;
					}
					PreviewComponent->MarkRenderStateDirty();
				}

				bool bSkeletonSockets = PreviewComponent->bSkeletonSocketsVisible != 0;
				if (TryReadBoolOption(Options, TEXT("skeleton_sockets"), bSkeletonSockets))
				{
					PreviewComponent->bSkeletonSocketsVisible = bSkeletonSockets;
					if (bSkeletonSockets)
					{
						PreviewComponent->bDrawSockets = true;
					}
					PreviewComponent->MarkRenderStateDirty();
				}
			}

			ApplyPersonaToggleBoolOption(*Client, Options, TEXT("normals"), &FAnimationViewportClient::IsSetShowNormalsChecked, &FAnimationViewportClient::ToggleShowNormals);
			ApplyPersonaToggleBoolOption(*Client, Options, TEXT("tangents"), &FAnimationViewportClient::IsSetShowTangentsChecked, &FAnimationViewportClient::ToggleShowTangents);
			ApplyPersonaToggleBoolOption(*Client, Options, TEXT("binormals"), &FAnimationViewportClient::IsSetShowBinormalsChecked, &FAnimationViewportClient::ToggleShowBinormals);

			bool bRealtime = Client->IsRealtime();
			if (TryGetBoolField(Options, TEXT("realtime"), bRealtime))
			{
				Client->SetRealtime(bRealtime);
			}

			Client->Invalidate();
			if (Client->Viewport)
			{
				Client->Viewport->Invalidate();
			}
			PreviewScene->InvalidateViews();

			OutData = MakePersonaVisualOptionsJson(*Client, PreviewComponent);
			if (Unsupported.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> UnsupportedJson;
				for (const FString& Key : Unsupported)
				{
					UnsupportedJson.Add(MakeShared<FJsonValueString>(Key));
				}
				OutData->SetArrayField(TEXT("unsupported_options"), UnsupportedJson);
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
			FAnimationViewportClient* Client = AsPersonaClient(Viewport);
			if (!Client)
			{
				OutError = TEXT("invalid_persona_viewport");
				return false;
			}
			const TSharedRef<IPersonaPreviewScene> PreviewScene = Client->GetPreviewScene();
			const TSharedRef<IPersonaToolkit> PersonaToolkit = PreviewScene->GetPersonaToolkit();
			if (!HasUserFields(PreviewState))
			{
				OutData = MakeShared<FJsonObject>();
				OutData->SetBoolField(TEXT("applied"), false);
				OutData->SetStringField(TEXT("reason"), TEXT("empty_preview_state"));
				return true;
			}

			bool bApplied = false;
			bool bShowReferencePose = PreviewScene->IsShowReferencePoseEnabled();
			if (TryGetBoolField(PreviewState, TEXT("show_reference_pose"), bShowReferencePose))
			{
				bool bResetBoneTransforms = false;
				TryGetBoolField(PreviewState, TEXT("reset_bone_transforms"), bResetBoneTransforms);
				PreviewScene->ShowReferencePose(bShowReferencePose, bResetBoneTransforms);
				PreviewScene->InvalidateViews();
				bApplied = true;
			}

			OutData = MakePersonaPreviewStateJson(*PersonaToolkit, *PreviewScene);
			OutData->SetBoolField(TEXT("applied"), bApplied);
			if (!bApplied)
			{
				OutError = TEXT("editor_viewport_preview_state_not_supported");
				return false;
			}
			return true;
		}

		virtual bool CaptureScreenshot(const FResolvedViewport& Viewport, const TSharedPtr<FJsonObject>& Params, FViewportScreenshotResult& OutResult, FString& OutError) const override
		{
			if (!AsPersonaClient(Viewport))
			{
				OutError = TEXT("invalid_persona_viewport");
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
			OutResult.CaptureBackend = TEXT("persona_editor_backbuffer");
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

	private:
		FString AdapterName;
		bool bControlRigOnly = false;

		static bool IsSkeletalMeshAssetType(const FString& AssetType)
		{
			const FString Value = NormalizeToken(AssetType);
			return Value == TEXT("skeletal_mesh") || Value == TEXT("skeletalmesh") || Value == TEXT("uskeletalmesh");
		}

		static bool IsSkeletonAssetType(const FString& AssetType)
		{
			const FString Value = NormalizeToken(AssetType);
			return Value == TEXT("skeleton") || Value == TEXT("uskeleton");
		}

		static bool IsAnimationAssetType(const FString& AssetType)
		{
			const FString Value = NormalizeToken(AssetType);
			return Value == TEXT("animation")
				|| Value == TEXT("animation_asset")
				|| Value == TEXT("animationasset")
				|| Value == TEXT("uanimationasset")
				|| Value == TEXT("anim_sequence")
				|| Value == TEXT("animsequence")
				|| Value == TEXT("uanimsequence")
				|| Value == TEXT("anim_montage")
				|| Value == TEXT("animmontage")
				|| Value == TEXT("uanimmontage")
				|| Value == TEXT("anim_composite")
				|| Value == TEXT("animcomposite");
		}

		static bool IsAnimBlueprintAssetType(const FString& AssetType)
		{
			const FString Value = NormalizeToken(AssetType);
			return Value == TEXT("anim_blueprint")
				|| Value == TEXT("animblueprint")
				|| Value == TEXT("animation_blueprint")
				|| Value == TEXT("animationblueprint")
				|| Value == TEXT("uanimblueprint");
		}

		static bool IsControlRigAssetType(const FString& AssetType)
		{
			const FString Value = NormalizeToken(AssetType);
			return Value == TEXT("control_rig")
				|| Value == TEXT("controlrig")
				|| Value == TEXT("control_rig_blueprint")
				|| Value == TEXT("controlrigblueprint")
				|| Value == TEXT("ucontrolrigblueprint");
		}

		static bool IsSupportedPersonaAssetType(const FString& AssetType)
		{
			return IsSkeletalMeshAssetType(AssetType)
				|| IsSkeletonAssetType(AssetType)
				|| IsAnimationAssetType(AssetType)
				|| IsAnimBlueprintAssetType(AssetType);
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

		static UObject* ResolvePersonaAsset(const FString& AssetPath, const FString& AssetType, FString& OutCanonicalAssetType, FString& OutError)
		{
			if (IsControlRigAssetType(AssetType))
			{
				OutError = TEXT("control_rig_requires_dedicated_viewport_adapter");
				return nullptr;
			}
			if (IsSkeletalMeshAssetType(AssetType))
			{
				if (USkeletalMesh* Mesh = LoadAssetByPath<USkeletalMesh>(AssetPath))
				{
					OutCanonicalAssetType = TEXT("skeletal_mesh");
					return Mesh;
				}
				OutError = TEXT("skeletal_mesh_not_found");
				return nullptr;
			}
			if (IsSkeletonAssetType(AssetType))
			{
				if (USkeleton* Skeleton = LoadAssetByPath<USkeleton>(AssetPath))
				{
					OutCanonicalAssetType = TEXT("skeleton");
					return Skeleton;
				}
				OutError = TEXT("skeleton_not_found");
				return nullptr;
			}
			if (IsAnimBlueprintAssetType(AssetType))
			{
				if (UAnimBlueprint* AnimBlueprint = LoadAssetByPath<UAnimBlueprint>(AssetPath))
				{
					OutCanonicalAssetType = TEXT("anim_blueprint");
					return AnimBlueprint;
				}
				OutError = TEXT("anim_blueprint_not_found");
				return nullptr;
			}
			if (IsAnimationAssetType(AssetType))
			{
				if (UAnimationAsset* AnimationAsset = LoadAssetByPath<UAnimationAsset>(AssetPath))
				{
					OutCanonicalAssetType = TEXT("animation_asset");
					return AnimationAsset;
				}
				OutError = TEXT("animation_asset_not_found");
				return nullptr;
			}
			if (!AssetType.IsEmpty())
			{
				OutError = TEXT("unsupported_persona_asset_type");
				return nullptr;
			}

			if (USkeletalMesh* Mesh = LoadAssetByPath<USkeletalMesh>(AssetPath))
			{
				OutCanonicalAssetType = TEXT("skeletal_mesh");
				return Mesh;
			}
			if (USkeleton* Skeleton = LoadAssetByPath<USkeleton>(AssetPath))
			{
				OutCanonicalAssetType = TEXT("skeleton");
				return Skeleton;
			}
			if (UAnimBlueprint* AnimBlueprint = LoadAssetByPath<UAnimBlueprint>(AssetPath))
			{
				OutCanonicalAssetType = TEXT("anim_blueprint");
				return AnimBlueprint;
			}
			if (UAnimationAsset* AnimationAsset = LoadAssetByPath<UAnimationAsset>(AssetPath))
			{
				OutCanonicalAssetType = TEXT("animation_asset");
				return AnimationAsset;
			}

			OutError = TEXT("persona_asset_not_found");
			return nullptr;
		}

		static UControlRigBlueprint* ResolveControlRigAsset(const FString& AssetPath, const FString& AssetType, FString& OutError)
		{
			if (!AssetType.IsEmpty() && !IsControlRigAssetType(AssetType))
			{
				OutError = TEXT("unsupported_control_rig_asset_type");
				return nullptr;
			}
			UControlRigBlueprint* ControlRigBlueprint = LoadAssetByPath<UControlRigBlueprint>(AssetPath);
			if (!ControlRigBlueprint)
			{
				OutError = TEXT("control_rig_asset_not_found");
				return nullptr;
			}
			return ControlRigBlueprint;
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

		static TSharedPtr<SEditorViewport> ResolvePersonaPreviewViewportWidget(FAssetEditorToolkit& Toolkit, FString& OutError)
		{
			const TSharedPtr<FTabManager> TabManager = Toolkit.GetAssociatedTabManager();
			if (!TabManager.IsValid())
			{
				OutError = TEXT("control_rig_tab_manager_not_found");
				return nullptr;
			}

			const TArray<FName> CandidateTabIds = { FPersonaTabs::PreviewViewportID, FPersonaTabs::PreviewViewport1ID, FPersonaTabs::PreviewViewport2ID, FPersonaTabs::PreviewViewport3ID };

			auto ResolveViewportFromTab = [](const TSharedPtr<SDockTab>& ViewportTab) -> TSharedPtr<SEditorViewport>
			{
				if (!ViewportTab.IsValid())
				{
					return nullptr;
				}
				const TSharedPtr<SWidget> RootWidget = ViewportTab->GetContent();
				if (!RootWidget.IsValid())
				{
					return nullptr;
				}
				TSharedPtr<SWidget> FoundWidget;
				if (!FindWidgetByTypeRecursive(RootWidget.ToSharedRef(), FName(TEXT("SAnimationEditorViewport")), FoundWidget))
				{
					return nullptr;
				}
				const TSharedPtr<SEditorViewport> ViewportWidget = StaticCastSharedPtr<SEditorViewport>(FoundWidget);
				return ViewportWidget.IsValid() && ViewportWidget->GetViewportClient().IsValid() ? ViewportWidget : nullptr;
			};

			for (const FName& TabId : CandidateTabIds)
			{
				const TSharedPtr<SEditorViewport> ViewportWidget = ResolveViewportFromTab(TabManager->FindExistingLiveTab(FTabId(TabId)));
				if (ViewportWidget.IsValid())
				{
					return ViewportWidget;
				}
			}
			for (const FName& TabId : CandidateTabIds)
			{
				const TSharedPtr<SDockTab> ViewportTab = TabManager->TryInvokeTab(FTabId(TabId), false);
				if (ViewportTab.IsValid())
				{
					ViewportTab->ActivateInParent(ETabActivationCause::SetDirectly);
					if (FSlateApplication::IsInitialized())
					{
						FSlateApplication::Get().Tick();
					}
				}
				const TSharedPtr<SEditorViewport> ViewportWidget = ResolveViewportFromTab(ViewportTab);
				if (ViewportWidget.IsValid())
				{
					return ViewportWidget;
				}
			}

			OutError = TEXT("control_rig_persona_viewport_widget_not_found");
			return nullptr;
		}

		bool ResolveControlRigViewport(const FResolveRequest& Request, FResolvedViewport& OutViewport, FString& OutError) const
		{
			UControlRigBlueprint* ControlRigBlueprint = ResolveControlRigAsset(Request.Target.AssetPath, Request.Target.AssetType, OutError);
			if (!ControlRigBlueprint)
			{
				return false;
			}
			if (!GEditor)
			{
				OutError = TEXT("editor_unavailable");
				return false;
			}

			UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
			if (!AssetEditorSubsystem)
			{
				OutError = TEXT("asset_editor_subsystem_unavailable");
				return false;
			}
			if (Request.Target.bOpenEditorIfNeeded)
			{
				AssetEditorSubsystem->OpenEditorForAsset(ControlRigBlueprint);
				if (FSlateApplication::IsInitialized())
				{
					FSlateApplication::Get().Tick();
				}
			}

			IAssetEditorInstance* EditorInstance = AssetEditorSubsystem->FindEditorForAsset(ControlRigBlueprint, false);
			if (!EditorInstance)
			{
				OutError = TEXT("control_rig_editor_not_opened");
				return false;
			}
			if (EditorInstance->GetEditorName() != FName(TEXT("ControlRigEditor")))
			{
				OutError = FString::Printf(TEXT("unsupported_control_rig_editor:%s"), *EditorInstance->GetEditorName().ToString());
				return false;
			}

			FAssetEditorToolkit* Toolkit = static_cast<FAssetEditorToolkit*>(EditorInstance);
			if (!Toolkit || !ToolkitEditsAsset(*Toolkit, ControlRigBlueprint))
			{
				OutError = TEXT("control_rig_editor_asset_mismatch");
				return false;
			}

			const TSharedPtr<SEditorViewport> ViewportWidget = ResolvePersonaPreviewViewportWidget(*Toolkit, OutError);
			if (!ViewportWidget.IsValid())
			{
				return false;
			}
			const TSharedPtr<FEditorViewportClient> ViewportClient = ViewportWidget->GetViewportClient();
			const TSharedPtr<FAnimationViewportClient> AnimationClient = StaticCastSharedPtr<FAnimationViewportClient>(ViewportClient);
			if (!AnimationClient.IsValid())
			{
				OutError = TEXT("control_rig_animation_viewport_client_not_found");
				return false;
			}
			if (!ToolkitEditsAsset(AnimationClient->GetAssetEditorToolkit().Get(), ControlRigBlueprint))
			{
				OutError = TEXT("control_rig_viewport_asset_mismatch");
				return false;
			}

			OutViewport.Target = Request.Target;
			OutViewport.Target.Scope = TEXT("asset_editor");
			OutViewport.Target.AssetPath = ControlRigBlueprint->GetPathName();
			OutViewport.Target.AssetType = TEXT("control_rig");
			OutViewport.Target.ViewportRole = TEXT("primary");
			OutViewport.Target.ViewportId = FString::Printf(TEXT("asset:%s#control_rig_primary"), *ControlRigBlueprint->GetPathName());
			OutViewport.AdapterName = GetName();
			OutViewport.ClientClass = TEXT("FAnimationViewportClient");
			OutViewport.ViewportClient = AnimationClient.Get();
			OutViewport.Asset = ControlRigBlueprint;
			return true;
		}

		static IHasPersonaToolkit* CastPersonaEditorInstance(IAssetEditorInstance* EditorInstance, FString& OutError)
		{
			if (!EditorInstance)
			{
				OutError = TEXT("persona_editor_not_opened");
				return nullptr;
			}

			const FName EditorName = EditorInstance->GetEditorName();
			FAssetEditorToolkit* Toolkit = static_cast<FAssetEditorToolkit*>(EditorInstance);
			if (!Toolkit)
			{
				return nullptr;
			}
			if (EditorName == FName(TEXT("SkeletalMeshEditor")))
			{
				return static_cast<IHasPersonaToolkit*>(static_cast<ISkeletalMeshEditor*>(Toolkit));
			}
			if (EditorName == FName(TEXT("SkeletonEditor")))
			{
				return static_cast<IHasPersonaToolkit*>(static_cast<ISkeletonEditor*>(Toolkit));
			}
			if (EditorName == FName(TEXT("AnimationEditor")))
			{
				return static_cast<IHasPersonaToolkit*>(static_cast<IAnimationEditor*>(Toolkit));
			}
			if (EditorName == FName(TEXT("AnimationBlueprintEditor")))
			{
				return static_cast<IHasPersonaToolkit*>(static_cast<IAnimationBlueprintEditor*>(Toolkit));
			}

			OutError = FString::Printf(TEXT("unsupported_persona_editor:%s"), *EditorName.ToString());
			return nullptr;
		}

		static IHasPersonaToolkit* GetPersonaHostFromAsset(UObject* Asset, const FString& CanonicalAssetType, const bool bOpenIfNeeded, FString& OutError)
		{
			if (!Asset || !GEditor)
			{
				OutError = TEXT("invalid_persona_asset");
				return nullptr;
			}

			if (CanonicalAssetType == TEXT("control_rig"))
			{
				OutError = TEXT("control_rig_requires_dedicated_viewport_adapter");
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
			IAssetEditorInstance* EditorInstance = AssetEditorSubsystem->FindEditorForAsset(Asset, false);
			if (!EditorInstance)
			{
				OutError = FString::Printf(TEXT("%s_editor_not_opened"), *CanonicalAssetType);
				return nullptr;
			}
			return CastPersonaEditorInstance(EditorInstance, OutError);
		}

		static IHasPersonaToolkit* GetPersonaHostFromResolved(const FResolvedViewport& Viewport, FString& OutError)
		{
			UObject* Asset = Viewport.Asset;
			if (!Asset)
			{
				OutError = TEXT("invalid_persona_asset");
				return nullptr;
			}
			return GetPersonaHostFromAsset(Asset, Viewport.Target.AssetType, false, OutError);
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

		static FAnimationViewportClient* AsPersonaClient(const FResolvedViewport& Viewport)
		{
			return static_cast<FAnimationViewportClient*>(Viewport.ViewportClient);
		}

		static FBox MakeComponentBoundsBox(const USkinnedMeshComponent* Component)
		{
			const FBoxSphereBounds Bounds = Component ? Component->Bounds : FBoxSphereBounds(EForceInit::ForceInit);
			return FBox(Bounds.Origin - Bounds.BoxExtent, Bounds.Origin + Bounds.BoxExtent);
		}

		static bool BuildSelectionFocusBox(IPersonaPreviewScene& PreviewScene, UDebugSkelMeshComponent& PreviewComponent, FBox& OutBox)
		{
			const FSelectedSocketInfo SelectedSocket = PreviewScene.GetSelectedSocket();
			if (SelectedSocket.IsValid() && SelectedSocket.Socket)
			{
				const FVector SocketLocation = SelectedSocket.Socket->GetSocketTransform(&PreviewComponent).GetLocation();
				OutBox = FBox::BuildAABB(SocketLocation, FVector(25.0f));
				return true;
			}

			const int32 SelectedBoneIndex = PreviewScene.GetSelectedBoneIndex();
			if (SelectedBoneIndex != INDEX_NONE)
			{
				const FVector BoneLocation = PreviewComponent.GetBoneTransform(SelectedBoneIndex).GetLocation();
				OutBox = FBox::BuildAABB(BoneLocation, FVector(25.0f));
				return true;
			}

			OutBox = FBox(ForceInit);
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

		static TSharedPtr<FJsonObject> MakePersonaVisualOptionsJson(FAnimationViewportClient& Client, const UDebugSkelMeshComponent* PreviewComponent)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("view_mode"), ViewModeToString(Client.GetViewMode()));
			Obj->SetNumberField(TEXT("view_mode_value"), static_cast<int32>(Client.GetViewMode()));
			TSharedPtr<FJsonObject> Overlays = MakeShared<FJsonObject>();
			Overlays->SetBoolField(TEXT("grid"), Client.IsSetShowGridChecked());
			Overlays->SetBoolField(TEXT("bounds"), Client.IsSetShowBoundsChecked());
			Overlays->SetBoolField(TEXT("bones"), Client.GetBoneDrawMode() != EBoneDrawMode::None);
			Overlays->SetStringField(TEXT("bone_draw_mode"), BoneDrawModeToString(Client.GetBoneDrawMode()));
			Overlays->SetBoolField(TEXT("local_axes"), Client.GetLocalAxesMode() != ELocalAxesMode::None);
			Overlays->SetStringField(TEXT("local_axes_mode"), LocalAxesModeToString(Client.GetLocalAxesMode()));
			Overlays->SetBoolField(TEXT("sockets"), PreviewComponent && PreviewComponent->bDrawSockets != 0);
			Overlays->SetBoolField(TEXT("mesh_sockets"), PreviewComponent && PreviewComponent->bMeshSocketsVisible != 0);
			Overlays->SetBoolField(TEXT("skeleton_sockets"), PreviewComponent && PreviewComponent->bSkeletonSocketsVisible != 0);
			Overlays->SetBoolField(TEXT("normals"), Client.IsSetShowNormalsChecked());
			Overlays->SetBoolField(TEXT("tangents"), Client.IsSetShowTangentsChecked());
			Overlays->SetBoolField(TEXT("binormals"), Client.IsSetShowBinormalsChecked());
			Obj->SetObjectField(TEXT("overlays"), Overlays);
			Obj->SetBoolField(TEXT("realtime"), Client.IsRealtime());
			return Obj;
		}

		static TSharedPtr<FJsonObject> MakePersonaPreviewStateJson(IPersonaToolkit& PersonaToolkit, IPersonaPreviewScene& PreviewScene)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			if (USkeletalMesh* Mesh = PersonaToolkit.GetMesh())
			{
				Obj->SetStringField(TEXT("asset_path"), Mesh->GetPathName());
			}
			if (USkeleton* Skeleton = PersonaToolkit.GetSkeleton())
			{
				Obj->SetStringField(TEXT("skeleton_path"), Skeleton->GetPathName());
			}
			if (USkeletalMesh* PreviewMesh = PreviewScene.GetPreviewMesh())
			{
				Obj->SetStringField(TEXT("preview_mesh_path"), PreviewMesh->GetPathName());
			}
			if (UAnimationAsset* AnimationAsset = PersonaToolkit.GetAnimationAsset())
			{
				Obj->SetStringField(TEXT("animation_asset_path"), AnimationAsset->GetPathName());
			}
			if (UAnimBlueprint* AnimBlueprint = PersonaToolkit.GetAnimBlueprint())
			{
				Obj->SetStringField(TEXT("anim_blueprint_path"), AnimBlueprint->GetPathName());
			}
			Obj->SetStringField(TEXT("context"), PersonaToolkit.GetContext().ToString());
			Obj->SetBoolField(TEXT("show_reference_pose"), PreviewScene.IsShowReferencePoseEnabled());
			const int32 SelectedBoneIndex = PreviewScene.GetSelectedBoneIndex();
			Obj->SetNumberField(TEXT("selected_bone_index"), SelectedBoneIndex);
			if (UDebugSkelMeshComponent* PreviewComponent = PreviewScene.GetPreviewMeshComponent())
			{
				if (SelectedBoneIndex != INDEX_NONE)
				{
					Obj->SetStringField(TEXT("selected_bone_name"), PreviewComponent->GetBoneName(SelectedBoneIndex).ToString());
				}
			}
			const FSelectedSocketInfo SelectedSocket = PreviewScene.GetSelectedSocket();
			if (SelectedSocket.IsValid() && SelectedSocket.Socket)
			{
				Obj->SetStringField(TEXT("selected_socket_name"), SelectedSocket.Socket->SocketName.ToString());
			}
			return Obj;
		}

		typedef bool (FAnimationViewportClient::*FPersonaBoolGetter)() const;
		typedef void (FAnimationViewportClient::*FPersonaToggle)();

		static void ApplyPersonaToggleBoolOption(
			FAnimationViewportClient& Client,
			const TSharedPtr<FJsonObject>& Options,
			const FString& Key,
			FPersonaBoolGetter Getter,
			FPersonaToggle Toggle)
		{
			bool bValue = false;
			if (TryReadBoolOption(Options, Key, bValue))
			{
				const bool bCurrent = (Client.*Getter)();
				if (bCurrent != bValue)
				{
					(Client.*Toggle)();
				}
			}
		}
	};

	TSharedRef<IViewportAdapter> MakePersonaViewportAdapter()
	{
		return MakeShared<FPersonaViewportAdapter>();
	}

	TSharedRef<IViewportAdapter> MakeControlRigPersonaViewportAdapter()
	{
		return MakeShared<FPersonaViewportAdapter>(TEXT("control_rig"), true);
	}
}
