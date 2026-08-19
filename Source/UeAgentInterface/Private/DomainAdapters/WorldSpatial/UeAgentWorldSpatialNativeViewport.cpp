#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialNativeInternal.h"

#include "Commands/Level/UeAgentHttpServer_LevelOps.h"

#include "Components/PrimitiveComponent.h"
#include "Editor.h"
#include "EditorModeManager.h"
#include "EditorViewportClient.h"
#include "Engine/Selection.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "InteractiveToolManager.h"
#include "InteractiveToolsContext.h"
#include "SceneView.h"
#include "Selection/GeometrySelectionManager.h"
#include "Selections/GeometrySelection.h"
#include "UnrealClient.h"

namespace UE::UAI::DomainAdapters::WorldSpatial::Private
{
	using namespace StateSemantic;
	using UE::Geometry::EGeometryElementType;
	using UE::Geometry::EGeometrySelectionChangeType;
	using UE::Geometry::EGeometryTopologyType;
	using UE::Geometry::FGeoSelectionID;
	using UE::Geometry::FGeometrySelection;
	using UE::Geometry::FGeometrySelectionUpdateConfig;
	using UE::Geometry::FGeometrySelectionUpdateResult;

	namespace
	{
		struct FViewportBinding
		{
			FString Identity;
			FEditorViewportClient* Client = nullptr;
			FViewport* Viewport = nullptr;
			UWorld* World = nullptr;
		};

		struct FViewportRay
		{
			FVector Origin = FVector::ZeroVector;
			FVector Direction = FVector::ForwardVector;
			FString EvidenceId;
		};

		struct FViewportStateObservation
		{
			FUeAgentWorldSpatialCompiledValue CameraTransform;
			double FovDegrees = 0.0;
			FString ViewMode;
			bool bRealtime = false;
			bool bGameView = false;
			TArray<FString> ShowFlags;
			FWorldSpatialViewportInteractionState Interaction;
			FWorldSpatialRevision Revision;
		};

		bool TryMakeStringValue(
			const FString& Value,
			const FUeAgentOperationContext& Context,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			FUeAgentStateSemanticError Error;
			return FUeAgentCanonicalValue::TryString(
				Value, Context.GetResourceTracker(), OutValue, Error)
				? true
				: Fail(
					OutError,
					TEXT("world_spatial_viewport_string_failed"),
					Value,
					Error.Code);
		}

		bool TryMakeBooleanValue(
			bool Value,
			const FUeAgentOperationContext& Context,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			FUeAgentStateSemanticError Error;
			return FUeAgentCanonicalValue::TryBoolean(
				Value, Context.GetResourceTracker(), OutValue, Error)
				? true
				: Fail(
					OutError,
					TEXT("world_spatial_viewport_boolean_failed"),
					FString(),
					Error.Code);
		}

		bool TryMakeStringArray(
			const TArray<FString>& Values,
			const FUeAgentOperationContext& Context,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			TArray<FUeAgentCanonicalValueRef> Items;
			for (const FString& Value : Values)
			{
				FUeAgentCanonicalValuePtr Item;
				if (!TryMakeStringValue(Value, Context, Item, OutError))
				{
					return false;
				}
				Items.Add(Item.ToSharedRef());
			}
			FUeAgentStateSemanticError Error;
			return FUeAgentCanonicalValue::TryArray(
				Items, Context.GetResourceTracker(), OutValue, Error)
				? true
				: Fail(
					OutError,
					TEXT("world_spatial_viewport_array_failed"),
					FString(),
					Error.Code);
		}

		bool TryMakeArray(
			const TArray<FUeAgentCanonicalValueRef>& Items,
			const FUeAgentOperationContext& Context,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			FUeAgentStateSemanticError Error;
			return FUeAgentCanonicalValue::TryArray(
				Items, Context.GetResourceTracker(), OutValue, Error)
				? true
				: Fail(
					OutError,
					TEXT("world_spatial_viewport_array_failed"),
					FString(),
					Error.Code);
		}

		bool TryMakeObject(
			const TArray<TPair<FString, FUeAgentCanonicalValueRef>>& Items,
			const FUeAgentOperationContext& Context,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			FUeAgentStateSemanticError Error;
			return FUeAgentCanonicalValue::TryObject(
				Items, Context.GetResourceTracker(), OutValue, Error)
				? true
				: Fail(
					OutError,
					TEXT("world_spatial_viewport_object_failed"),
					FString(),
					Error.Code);
		}

		bool TryMakeTemporalValue(
			const FUeAgentTemporalPoint& Value,
			const FUeAgentOperationContext& Context,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			FUeAgentStateSemanticError Error;
			return FUeAgentCanonicalValue::TryTemporal(
				Value, Context.GetResourceTracker(), OutValue, Error)
				? true
				: Fail(
					OutError,
					TEXT("world_spatial_viewport_temporal_failed"),
					FString(),
					Error.Code);
		}

		bool TryMakeIntegerArray(
			const TArray<int64>& Values,
			const FUeAgentOperationContext& Context,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			TArray<FUeAgentCanonicalValueRef> Items;
			for (const int64 Value : Values)
			{
				FUeAgentCanonicalValuePtr Item;
				if (!TryMakeIntegerValue(Value, Context, Item, OutError))
				{
					return false;
				}
				Items.Add(Item.ToSharedRef());
			}
			FUeAgentStateSemanticError Error;
			return FUeAgentCanonicalValue::TryArray(
				Items, Context.GetResourceTracker(), OutValue, Error)
				? true
				: Fail(
					OutError,
					TEXT("world_spatial_viewport_integer_array_failed"),
					FString(),
					Error.Code);
		}

		bool TryReadBooleanValue(
			const FUeAgentCanonicalSnapshot& Snapshot,
			const FString& Role,
			bool& OutValue,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			const FUeAgentStateEntry* Entry = FindEntryByRole(Snapshot, Role);
			if (!Entry || Entry->GetValue()->GetKind() != EUeAgentValueKind::Boolean)
			{
				return Fail(
					OutError,
					TEXT("world_spatial_viewport_boolean_readback_invalid"),
					Role);
			}
			OutValue = Entry->GetValue()->GetBoolean();
			return true;
		}

		bool TryReadStringValue(
			const FUeAgentCanonicalSnapshot& Snapshot,
			const FString& Role,
			FString& OutValue,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			const FUeAgentStateEntry* Entry = FindEntryByRole(Snapshot, Role);
			if (!Entry || Entry->GetValue()->GetKind() != EUeAgentValueKind::String)
			{
				return Fail(
					OutError,
					TEXT("world_spatial_viewport_string_readback_invalid"),
					Role);
			}
			OutValue = Entry->GetValue()->GetText();
			return true;
		}

		bool TryReadIntegerArray(
			const FUeAgentCanonicalSnapshot& Snapshot,
			const FString& Role,
			TArray<int64>& OutValues,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			OutValues.Reset();
			const FUeAgentStateEntry* Entry = FindEntryByRole(Snapshot, Role);
			if (!Entry || Entry->GetValue()->GetKind() != EUeAgentValueKind::Array)
			{
				return Fail(
					OutError,
					TEXT("world_spatial_viewport_integer_array_readback_invalid"),
					Role);
			}
			for (const FUeAgentCanonicalValueRef& Item
				: Entry->GetValue()->GetArrayItems())
			{
				int64 Parsed = 0;
				if (Item->GetKind() != EUeAgentValueKind::Integer
					|| !LexTryParseString(Parsed, *Item->GetLexeme()))
				{
					return Fail(
						OutError,
						TEXT("world_spatial_viewport_integer_array_item_invalid"),
						Role);
				}
				OutValues.Add(Parsed);
			}
			return true;
		}

		bool TryResolveViewportIdentity(
			IUeAgentWorldSpatialNativeEnvironment& Environment,
			const FString& Identity,
			FViewportBinding& Out,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			Out = {};
			Out.Identity = Identity;
			if (Out.Identity.IsEmpty()
				|| !Environment.TryResolveViewport(
					Out.Identity, Out.Client, Out.Viewport, OutError)
				|| !Out.Client || !Out.Viewport
				|| Out.Client->Viewport != Out.Viewport)
			{
				return OutError.IsValid()
					? false
					: Fail(
						OutError,
						TEXT("world_spatial_viewport_identity_unresolved"),
						Out.Identity);
			}
			Out.World = Out.Client->GetWorld();
			const FIntPoint Size = Out.Viewport->GetSizeXY();
			return Out.World && Out.World->Scene && Size.X > 0 && Size.Y > 0
				? true
				: Fail(
					OutError,
					TEXT("world_spatial_viewport_state_unavailable"),
					Out.Identity);
		}

		bool TryResolveViewportIdentityForCatalog(
			IUeAgentWorldSpatialNativeEnvironment& Environment,
			const FString& Identity,
			FViewportBinding& Out,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			Out = {};
			Out.Identity = Identity;
			if (Out.Identity.IsEmpty()
				|| !Environment.TryResolveViewport(
					Out.Identity,
					Out.Client,
					Out.Viewport,
					OutError)
				|| !Out.Client || !Out.Viewport
				|| Out.Client->Viewport != Out.Viewport)
			{
				return OutError.IsValid()
					? false
					: Fail(
						OutError,
						TEXT("world_spatial_viewport_identity_unresolved"),
						Out.Identity);
			}
			Out.World = Out.Client->GetWorld();
			return Out.World
				? true
				: Fail(
					OutError,
					TEXT("world_spatial_viewport_world_unavailable"),
					Out.Identity);
		}

		bool TryResolveViewport(
			IUeAgentWorldSpatialNativeEnvironment& Environment,
			const FUeAgentOperationContext& Context,
			FViewportBinding& Out,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			return TryResolveViewportIdentity(
				Environment,
				Context.GetExecutionContext().GetViewportIdentity(),
				Out,
				OutError);
		}

		bool TryBuildSceneView(
			const FViewportBinding& Binding,
			FSceneViewFamilyContext& ViewFamily,
			FSceneView*& OutView,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			OutView = Binding.Client->CalcSceneView(&ViewFamily);
			return OutView
				? true
				: Fail(
					OutError,
					TEXT("world_spatial_viewport_scene_view_failed"),
					Binding.Identity);
		}

		FSceneViewFamilyContext MakeViewFamily(const FViewportBinding& Binding)
		{
			return FSceneViewFamilyContext(FSceneViewFamily::ConstructionValues(
				Binding.Viewport,
				Binding.World->Scene,
				Binding.Client->EngineShowFlags)
				.SetRealtimeUpdate(Binding.Client->IsRealtime()));
		}

		bool TryDeproject(
			IUeAgentWorldSpatialSpaceCompiler& Compiler,
			const FWorldSpatialValue& ScreenPoint,
			const FViewportBinding& Binding,
			FViewportRay& OutRay,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			OutRay = {};
			if (ScreenPoint.Subject != Binding.Identity)
			{
				return Fail(
					OutError,
					TEXT("world_spatial_viewport_subject_mismatch"),
					ScreenPoint.Subject,
					Binding.Identity);
			}
			FUeAgentWorldSpatialCompiledValue Point;
			if (!Compiler.TryConvertToNative(
					ScreenPoint,
					TEXT("viewport.screen.pixel"),
					TEXT("pixel"),
					Point,
					OutError)
				|| Point.Dimension != 2 || Point.Quantity != TEXT("point"))
			{
				return false;
			}
			const FIntPoint Size = Binding.Viewport->GetSizeXY();
			if (Point.Components[0] < 0.0 || Point.Components[0] >= Size.X
				|| Point.Components[1] < 0.0 || Point.Components[1] >= Size.Y)
			{
				return Fail(
					OutError,
					TEXT("world_spatial_viewport_screen_point_out_of_bounds"),
					ScreenPoint.Subject);
			}
			FSceneViewFamilyContext ViewFamily = MakeViewFamily(Binding);
			FSceneView* View = nullptr;
			if (!TryBuildSceneView(Binding, ViewFamily, View, OutError))
			{
				return false;
			}
			View->DeprojectFVector2D(
				FVector2D(Point.Components[0], Point.Components[1]),
				OutRay.Origin,
				OutRay.Direction);
			OutRay.Direction = OutRay.Direction.GetSafeNormal();
			OutRay.EvidenceId = TEXT("viewport-deprojection-dimension-bridge:")
				+ Binding.Identity;
			return !OutRay.Direction.IsNearlyZero()
				? true
				: Fail(
					OutError,
					TEXT("world_spatial_viewport_deprojection_invalid"),
					Binding.Identity);
		}

		bool TryTraceChannel(
			const FString& Name,
			ECollisionChannel& OutChannel,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			if (Name == TEXT("visibility"))
			{
				OutChannel = ECC_Visibility;
				return true;
			}
			if (Name == TEXT("camera"))
			{
				OutChannel = ECC_Camera;
				return true;
			}
			return Fail(
				OutError,
				TEXT("world_spatial_viewport_trace_channel_unsupported"),
				Name);
		}

		bool TryTrace(
			IUeAgentWorldSpatialNativeEnvironment& Environment,
			const FViewportBinding& Binding,
			const FViewportRay& Ray,
			const FWorldSpatialTraceQuery& Query,
			FHitResult& OutHit,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			ECollisionChannel Channel = ECC_Visibility;
			if (!TryTraceChannel(Query.TraceChannel, Channel, OutError))
			{
				return false;
			}
			FCollisionQueryParams Params(
				TEXT("UAIWorldSpatialViewport"), Query.bTraceComplex);
			for (const FString& Ref : Query.IgnoreTargetRefs)
			{
				UObject* Object = nullptr;
				if (!Environment.TryResolveObject(Ref, Object, OutError))
				{
					return false;
				}
				if (AActor* Actor = Cast<AActor>(Object))
				{
					Params.AddIgnoredActor(Actor);
				}
				else if (UPrimitiveComponent* Component =
					Cast<UPrimitiveComponent>(Object))
				{
					Params.AddIgnoredComponent(Component);
				}
				else
				{
					return Fail(
						OutError,
						TEXT("world_spatial_viewport_trace_ignore_type_invalid"),
						Ref);
				}
			}
			OutHit = {};
			Binding.World->LineTraceSingleByChannel(
				OutHit,
				Ray.Origin,
				Ray.Origin + Ray.Direction * Query.MaxDistanceCm,
				Channel,
				Params);
			return true;
		}

		FWorldSpatialTraceQuery DefaultTraceQuery()
		{
			return {100000.0, TEXT("visibility"), true, {}};
		}

		FUeAgentWorldSpatialCompiledValue MakeVectorValue(
			const FString& Subject,
			const FString& Quantity,
			const FString& Frame,
			const FString& Unit,
			const FVector& Vector,
			const FString& Evidence)
		{
			return {
				Subject,
				Quantity,
				Frame,
				3,
				Unit,
				{Vector.X, Vector.Y, Vector.Z},
				Evidence,
			};
		}

		FString ViewModeToId(EViewModeIndex Mode)
		{
			switch (Mode)
			{
			case VMI_Lit: return TEXT("lit");
			case VMI_Unlit: return TEXT("unlit");
			case VMI_BrushWireframe:
			case VMI_Wireframe: return TEXT("wireframe");
			default: return FString::Printf(TEXT("engine:%d"), static_cast<int32>(Mode));
			}
		}

		FString ViewportViewModeId(FEditorViewportClient& Client)
		{
			const TSharedPtr<FJsonObject> State =
				UeAgentLevelOps::MakeCurrentViewportViewModeJson(
					&Client);
			FString Key;
			return State.IsValid()
				&& State->TryGetStringField(TEXT("key"), Key)
				&& !Key.IsEmpty()
					? Key
					: ViewModeToId(Client.GetViewMode());
		}

		bool TryApplyViewModeId(
			const FString& Id,
			FEditorViewportClient& Client,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("mode_key"), Id);
			bool bHasViewMode = false;
			UeAgentLevelOps::FResolvedViewportViewMode Resolved;
			FString Error;
			if (!UeAgentLevelOps::TryResolveRequestedViewportViewMode(
					Params, bHasViewMode, Resolved, Error)
				|| !bHasViewMode)
			{
				return Fail(
					OutError,
					TEXT("world_spatial_viewport_view_mode_unsupported"),
					Id,
					Error);
			}
			UeAgentLevelOps::ApplyResolvedViewportViewMode(&Client, Resolved);
			return true;
		}

		FString WidgetModeToId(const UE::Widget::EWidgetMode Mode)
		{
			switch (Mode)
			{
			case UE::Widget::WM_None: return TEXT("none");
			case UE::Widget::WM_Translate: return TEXT("translate");
			case UE::Widget::WM_Rotate: return TEXT("rotate");
			case UE::Widget::WM_Scale: return TEXT("scale");
			default: return FString::Printf(TEXT("engine:%d"), static_cast<int32>(Mode));
			}
		}

		bool TryApplyWidgetMode(
			const FString& Id,
			FEditorViewportClient& Client,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			const FString Normalized = Id.TrimStartAndEnd().ToLower();
			UE::Widget::EWidgetMode Mode = UE::Widget::WM_None;
			if (Normalized == TEXT("translate"))
			{
				Mode = UE::Widget::WM_Translate;
			}
			else if (Normalized == TEXT("rotate"))
			{
				Mode = UE::Widget::WM_Rotate;
			}
			else if (Normalized == TEXT("scale"))
			{
				Mode = UE::Widget::WM_Scale;
			}
			else if (Normalized != TEXT("none"))
			{
				return Fail(
					OutError,
					TEXT("world_spatial_viewport_widget_mode_unsupported"),
					Id);
			}
			FEditorModeTools* ModeTools = Client.GetModeTools();
			if (!ModeTools)
			{
				return Fail(
					OutError,
					TEXT("world_spatial_viewport_mode_tools_missing"),
					Id);
			}
			if (ModeTools->IsTracking())
			{
				return Fail(
					OutError,
					TEXT("world_spatial_viewport_widget_mode_tracking"),
					Id);
			}
			if (Mode != UE::Widget::WM_None && !Client.CanSetWidgetMode(Mode))
			{
				return Fail(
					OutError,
					TEXT("world_spatial_viewport_widget_mode_unavailable"),
					Id);
			}

			// FEditorViewportClient::SetWidgetMode intentionally ignores a hidden
			// viewport widget.  The mode manager is the actual state authority and
			// remains available to command-line Editor workers, so update it directly.
			ModeTools->SetWidgetMode(Mode);
			ModeTools->BroadcastWidgetModeChanged(Mode);
			return true;
		}

		void ApplyRealtimeState(bool bRealtime, FEditorViewportClient& Client)
		{
			const FText OverrideName = FText::FromString(
				TEXT("UeAgentInterface viewport state"));
			Client.RemoveRealtimeOverride(OverrideName, false);
			Client.SetRealtime(bRealtime);
			if (!Client.DoRealtimeAndOverridesMatch(bRealtime))
			{
				// Preserve other systems' overrides and layer the explicit public
				// operation above them.  A later UAI update/restore removes only this
				// named override before establishing the requested effective state.
				Client.AddRealtimeOverride(bRealtime, OverrideName);
			}
		}

		const TArray<FString>& CuratedShowFlags()
		{
			static const TArray<FString> Values{
				TEXT("Lighting"),
				TEXT("StaticMeshes"),
			};
			return Values;
		}

		bool TryReadShowFlags(
			const FEditorViewportClient& Client,
			TArray<FString>& OutFlags,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			OutFlags.Reset();
			for (const FString& Name : CuratedShowFlags())
			{
				const int32 Index = FEngineShowFlags::FindIndexByName(*Name);
				if (Index < 0)
				{
					return Fail(
						OutError,
						TEXT("world_spatial_viewport_show_flag_missing"),
						Name);
				}
				if (Client.EngineShowFlags.GetSingleFlag(Index))
				{
					OutFlags.Add(Name);
				}
			}
			return true;
		}

		bool TryApplyShowFlags(
			FEditorViewportClient& Client,
			const TArray<FString>& Enabled,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			for (const FString& Name : Enabled)
			{
				if (!CuratedShowFlags().Contains(Name))
				{
					return Fail(
						OutError,
						TEXT("world_spatial_viewport_show_flag_unsupported"),
						Name);
				}
			}
			for (const FString& Name : CuratedShowFlags())
			{
				const int32 Index = FEngineShowFlags::FindIndexByName(*Name);
				if (Index < 0)
				{
					return Fail(
						OutError,
						TEXT("world_spatial_viewport_show_flag_missing"),
						Name);
				}
				Client.EngineShowFlags.SetSingleFlag(Index, Enabled.Contains(Name));
			}
			return true;
		}

		FUeAgentWorldSpatialCompiledValue MakeCameraTransform(
			const FViewportBinding& Binding)
		{
			const FQuat Rotation = Binding.Client->GetViewRotation().Quaternion();
			const FVector Location = Binding.Client->GetViewLocation();
			return {
				Binding.Identity + TEXT(".camera"),
				TEXT("transform"),
				TEXT("unreal.world"),
				3,
				TEXT("centimeter"),
				{
					Location.X, Location.Y, Location.Z,
					Rotation.X, Rotation.Y, Rotation.Z, Rotation.W,
					1.0, 1.0, 1.0,
				},
				TEXT("same-viewport-camera-readback:" ) + Binding.Identity,
			};
		}

		FString ViewportTypeToId(const ELevelViewportType ViewportType)
		{
			switch (ViewportType)
			{
			case LVT_Perspective:
				return TEXT("perspective");
			case LVT_OrthoXY:
				return TEXT("orthographic_xy");
			case LVT_OrthoXZ:
				return TEXT("orthographic_xz");
			case LVT_OrthoYZ:
				return TEXT("orthographic_yz");
			case LVT_OrthoFreelook:
				return TEXT("orthographic_freelook");
			default:
				return TEXT("unknown");
			}
		}

		FString CameraFingerprint(const FViewportBinding& Binding)
		{
			const FVector Location = Binding.Client->GetViewLocation();
			const FRotator Rotation = Binding.Client->GetViewRotation();
			return FString::Printf(
				TEXT("%s|%.9g,%.9g,%.9g|%.9g,%.9g,%.9g|%.9g|%s"),
				*Binding.Identity,
				Location.X,
				Location.Y,
				Location.Z,
				Rotation.Pitch,
				Rotation.Yaw,
				Rotation.Roll,
				Binding.Client->ViewFOV,
				*ViewportViewModeId(*Binding.Client));
		}

		bool TryMakeViewportCameraValue(
			const FViewportBinding& Binding,
			const FUeAgentOperationContext& Context,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			FUeAgentCanonicalValuePtr Transform;
			FUeAgentCanonicalValuePtr Fov;
			FUeAgentCanonicalValuePtr Mode;
			FUeAgentCanonicalValuePtr ObservedAt;
			if (!TryMakeCompiledValue(
					MakeCameraTransform(Binding), Context, Transform, OutError)
				|| !TryMakeDecimalValue(
					Binding.Client->ViewFOV, Context, Fov, OutError)
				|| !TryMakeStringValue(
					ViewportViewModeId(*Binding.Client),
					Context,
					Mode,
					OutError)
				|| !TryMakeTemporalValue(
					FUeAgentTemporalPoint(
						TEXT("editor-native"),
						FString::Printf(
							TEXT("%llu"),
							FPlatformTime::Cycles64())),
					Context,
					ObservedAt,
					OutError))
			{
				return false;
			}
			return TryMakeObject(
				{
					{TEXT("camera_transform"), Transform.ToSharedRef()},
					{TEXT("fov_degrees"), Fov.ToSharedRef()},
					{TEXT("view_mode"), Mode.ToSharedRef()},
					{TEXT("observed_at"), ObservedAt.ToSharedRef()},
				},
				Context,
				OutValue,
				OutError);
		}

		bool TryMakeViewModeCatalog(
			const FUeAgentOperationContext& Context,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			const TSharedPtr<FJsonObject> Catalog =
				UeAgentLevelOps::MakeViewportViewModeCatalogJson();
			const TArray<TSharedPtr<FJsonValue>>* Modes = nullptr;
			if (!Catalog.IsValid()
				|| !Catalog->TryGetArrayField(TEXT("modes"), Modes)
				|| !Modes || Modes->IsEmpty())
			{
				return Fail(OutError,
					TEXT("world_spatial_viewport_view_mode_catalog_missing"),
					TEXT("modes"));
			}
			TArray<FUeAgentCanonicalValueRef> Items;
			for (const TSharedPtr<FJsonValue>& ModeValue : *Modes)
			{
				const TSharedPtr<FJsonObject>* Mode = nullptr;
				FString ModeId;
				FString ModeLabel;
				if (!ModeValue.IsValid()
					|| !ModeValue->TryGetObject(Mode)
					|| !Mode || !Mode->IsValid()
					|| !(*Mode)->TryGetStringField(TEXT("key"), ModeId)
					|| !(*Mode)->TryGetStringField(TEXT("label"), ModeLabel)
					|| ModeId.IsEmpty() || ModeLabel.IsEmpty())
				{
					return Fail(OutError,
						TEXT("world_spatial_viewport_view_mode_catalog_entry_invalid"),
						TEXT("mode"));
				}
				FUeAgentCanonicalValuePtr Id;
				FUeAgentCanonicalValuePtr Label;
				FUeAgentCanonicalValuePtr Item;
				if (!TryMakeStringValue(ModeId, Context, Id, OutError)
					|| !TryMakeStringValue(ModeLabel, Context, Label, OutError)
					|| !TryMakeObject(
						{
							{TEXT("mode_id"), Id.ToSharedRef()},
							{TEXT("label"), Label.ToSharedRef()},
						},
						Context,
						Item,
						OutError))
				{
					return false;
				}
				Items.Add(Item.ToSharedRef());
			}
			return TryMakeArray(Items, Context, OutValue, OutError);
		}

		bool TryMakeViewportWarnings(
			UWorld* World,
			const bool bIncludeSuppressed,
			const FUeAgentOperationContext& Context,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			TSharedPtr<FJsonObject> WarningData = MakeShared<FJsonObject>();
			FString Error;
			if (!UeAgentLevelOps::BuildViewportWarnings(
					World, bIncludeSuppressed, WarningData, Error))
			{
				return Fail(OutError,
					TEXT("world_spatial_viewport_warning_probe_failed"),
					TEXT("warnings"), Error);
			}
			const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
			if (!WarningData->TryGetArrayField(TEXT("warnings"), Warnings)
				|| !Warnings)
			{
				return Fail(OutError,
					TEXT("world_spatial_viewport_warning_probe_invalid"),
					TEXT("warnings"));
			}
			TArray<FUeAgentCanonicalValueRef> Items;
			for (const TSharedPtr<FJsonValue>& WarningValue : *Warnings)
			{
				const TSharedPtr<FJsonObject>* Warning = nullptr;
				FString Code;
				FString Message;
				FString Severity;
				if (!WarningValue.IsValid()
					|| !WarningValue->TryGetObject(Warning)
					|| !Warning || !Warning->IsValid()
					|| !(*Warning)->TryGetStringField(TEXT("code"), Code)
					|| !(*Warning)->TryGetStringField(TEXT("text"), Message)
					|| !(*Warning)->TryGetStringField(TEXT("severity"), Severity))
				{
					return Fail(OutError,
						TEXT("world_spatial_viewport_warning_entry_invalid"),
						TEXT("warning"));
				}
				FUeAgentCanonicalValuePtr CodeValue;
				FUeAgentCanonicalValuePtr MessageValue;
				FUeAgentCanonicalValuePtr SeverityValue;
				FUeAgentCanonicalValuePtr Item;
				if (!TryMakeStringValue(Code, Context, CodeValue, OutError)
					|| !TryMakeStringValue(Message, Context, MessageValue, OutError)
					|| !TryMakeStringValue(Severity, Context, SeverityValue, OutError)
					|| !TryMakeObject(
						{
							{TEXT("code"), CodeValue.ToSharedRef()},
							{TEXT("message"), MessageValue.ToSharedRef()},
							{TEXT("severity"), SeverityValue.ToSharedRef()},
						},
						Context, Item, OutError))
				{
					return false;
				}
				Items.Add(Item.ToSharedRef());
			}
			return TryMakeArray(Items, Context, OutValue, OutError);
		}

		bool TryObserveViewportState(
			IUeAgentWorldSpatialRevisionSource& RevisionSource,
			const FViewportBinding& Binding,
			FViewportStateObservation& Out,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			Out = {};
			Out.CameraTransform = MakeCameraTransform(Binding);
			Out.FovDegrees = Binding.Client->ViewFOV;
			Out.ViewMode = ViewportViewModeId(*Binding.Client);
			Out.bRealtime = Binding.Client->IsRealtime();
			Out.bGameView = Binding.Client->IsInGameView();
			Out.Interaction.bModeWidgets = Binding.Client->EngineShowFlags.ModeWidgets != 0;
			Out.Interaction.bSelection = Binding.Client->EngineShowFlags.Selection != 0;
			Out.Interaction.bPivot = Binding.Client->EngineShowFlags.Pivot != 0;
			Out.Interaction.bEditorPrimitives = Binding.Client->EngineShowFlags.Editor != 0;
			Out.Interaction.bGamePrimitives = Binding.Client->EngineShowFlags.Game != 0;
			Out.Interaction.WidgetMode = WidgetModeToId(
				Binding.Client->GetWidgetMode());
			if (!Out.Interaction.IsValid())
			{
				return Fail(OutError,
					TEXT("world_spatial_viewport_interaction_readback_invalid"),
					Binding.Identity);
			}
			Out.Interaction.WidgetMode.ToLowerInline();
			return Out.CameraTransform.IsValid()
				&& TryReadShowFlags(*Binding.Client, Out.ShowFlags, OutError)
				&& RevisionSource.TryReadRevision(
					Binding.Identity, Out.Revision, OutError)
				&& Out.Revision.IsValid();
		}

		bool TryMakeViewportStateSnapshot(
			const FUeAgentWorldSpatialExecutionChainContract& Contract,
			EUeAgentWorldSpatialReadPhase Phase,
			const FViewportBinding& Binding,
			IUeAgentWorldSpatialRevisionSource& RevisionSource,
			const FUeAgentOperationContext& Context,
			TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			FViewportStateObservation State;
			if (!TryObserveViewportState(
					RevisionSource, Binding, State, OutError))
			{
				return false;
			}
			FUeAgentCanonicalValuePtr Camera;
			FUeAgentCanonicalValuePtr Fov;
			FUeAgentCanonicalValuePtr Mode;
			FUeAgentCanonicalValuePtr Realtime;
			FUeAgentCanonicalValuePtr GameView;
			FUeAgentCanonicalValuePtr Flags;
			FUeAgentCanonicalValuePtr ModeWidgets;
			FUeAgentCanonicalValuePtr Selection;
			FUeAgentCanonicalValuePtr Pivot;
			FUeAgentCanonicalValuePtr EditorPrimitives;
			FUeAgentCanonicalValuePtr GamePrimitives;
			FUeAgentCanonicalValuePtr WidgetMode;
			FUeAgentCanonicalValuePtr RevisionSourceValue;
			FUeAgentCanonicalValuePtr RevisionGenerationValue;
			FUeAgentCanonicalValuePtr RevisionSequenceValue;
			if (!TryMakeCompiledValue(
					State.CameraTransform, Context, Camera, OutError)
				|| !TryMakeDecimalValue(State.FovDegrees, Context, Fov, OutError)
				|| !TryMakeStringValue(State.ViewMode, Context, Mode, OutError)
				|| !TryMakeBooleanValue(State.bRealtime, Context, Realtime, OutError)
				|| !TryMakeBooleanValue(State.bGameView, Context, GameView, OutError)
				|| !TryMakeStringArray(State.ShowFlags, Context, Flags, OutError)
				|| !TryMakeBooleanValue(State.Interaction.bModeWidgets, Context, ModeWidgets, OutError)
				|| !TryMakeBooleanValue(State.Interaction.bSelection, Context, Selection, OutError)
				|| !TryMakeBooleanValue(State.Interaction.bPivot, Context, Pivot, OutError)
				|| !TryMakeBooleanValue(State.Interaction.bEditorPrimitives, Context, EditorPrimitives, OutError)
				|| !TryMakeBooleanValue(State.Interaction.bGamePrimitives, Context, GamePrimitives, OutError)
				|| !TryMakeStringValue(State.Interaction.WidgetMode, Context, WidgetMode, OutError)
				|| !TryMakeStringValue(
					State.Revision.SourceId, Context, RevisionSourceValue, OutError)
				|| !TryMakeStringValue(
					State.Revision.Generation, Context, RevisionGenerationValue, OutError)
				|| !TryMakeIntegerValue(
					State.Revision.Sequence, Context, RevisionSequenceValue, OutError))
			{
				return false;
			}
			const FString Owner = Binding.Identity;
			return TryMakeSnapshot(
				Contract,
				Phase,
				FString::Printf(
					TEXT("%s|%s|%.9g|%d|%d|%s|%d,%d,%d,%d,%d,%s|%s|%s|%lld"),
					*Owner,
					*State.CameraTransform.EvidenceId,
					State.FovDegrees,
					State.bRealtime,
					State.bGameView,
					*FString::Join(State.ShowFlags, TEXT(",")),
					State.Interaction.bModeWidgets,
					State.Interaction.bSelection,
					State.Interaction.bPivot,
					State.Interaction.bEditorPrimitives,
					State.Interaction.bGamePrimitives,
					*State.Interaction.WidgetMode,
					*State.Revision.SourceId,
					*State.Revision.Generation,
					State.Revision.Sequence),
				{
					{TEXT("world-spatial:viewport:camera"), TEXT("viewport"), Owner, Owner, TEXT("viewport_state.camera_transform"), TEXT("world_spatial.compiled_value.v1"), Camera},
					{TEXT("world-spatial:viewport:fov"), TEXT("viewport"), Owner, Owner, TEXT("viewport_state.fov"), TEXT("core.decimal"), Fov},
					{TEXT("world-spatial:viewport:mode"), TEXT("viewport"), Owner, Owner, TEXT("viewport_state.view_mode"), TEXT("core.string"), Mode},
					{TEXT("world-spatial:viewport:realtime"), TEXT("viewport"), Owner, Owner, TEXT("viewport_state.realtime"), TEXT("core.boolean"), Realtime},
					{TEXT("world-spatial:viewport:game-view"), TEXT("viewport"), Owner, Owner, TEXT("viewport_state.game_view"), TEXT("core.boolean"), GameView},
					{TEXT("world-spatial:viewport:show-flags"), TEXT("viewport"), Owner, Owner, TEXT("viewport_state.show_flags"), TEXT("core.string_array"), Flags},
					{TEXT("world-spatial:viewport:mode-widgets"), TEXT("viewport"), Owner, Owner, TEXT("viewport_state.interaction.mode_widgets"), TEXT("core.boolean"), ModeWidgets},
					{TEXT("world-spatial:viewport:selection"), TEXT("viewport"), Owner, Owner, TEXT("viewport_state.interaction.selection"), TEXT("core.boolean"), Selection},
					{TEXT("world-spatial:viewport:pivot"), TEXT("viewport"), Owner, Owner, TEXT("viewport_state.interaction.pivot"), TEXT("core.boolean"), Pivot},
					{TEXT("world-spatial:viewport:editor-primitives"), TEXT("viewport"), Owner, Owner, TEXT("viewport_state.interaction.editor_primitives"), TEXT("core.boolean"), EditorPrimitives},
					{TEXT("world-spatial:viewport:game-primitives"), TEXT("viewport"), Owner, Owner, TEXT("viewport_state.interaction.game_primitives"), TEXT("core.boolean"), GamePrimitives},
					{TEXT("world-spatial:viewport:widget-mode"), TEXT("viewport"), Owner, Owner, TEXT("viewport_state.interaction.widget_mode"), TEXT("core.string"), WidgetMode},
					{TEXT("world-spatial:viewport:revision-source"), TEXT("viewport"), Owner, Owner, TEXT("viewport_state.revision.source_id"), TEXT("core.string"), RevisionSourceValue},
					{TEXT("world-spatial:viewport:revision-generation"), TEXT("viewport"), Owner, Owner, TEXT("viewport_state.revision.generation"), TEXT("core.string"), RevisionGenerationValue},
					{TEXT("world-spatial:viewport:revision-sequence"), TEXT("viewport"), Owner, Owner, TEXT("viewport_state.revision.sequence"), TEXT("core.integer"), RevisionSequenceValue},
				},
				Context,
				OutSnapshot,
				OutError);
		}

		bool TryApplyCameraTransform(
			IUeAgentWorldSpatialSpaceCompiler& Compiler,
			const FWorldSpatialValue& Transform,
			FEditorViewportClient& Client,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			FUeAgentWorldSpatialCompiledValue Compiled;
			if (!Compiler.TryConvertToNative(
					Transform,
					TEXT("unreal.world"),
					TEXT("centimeter"),
					Compiled,
					OutError)
				|| Compiled.Quantity != TEXT("transform")
				|| Compiled.Dimension != 3)
			{
				return false;
			}
			const FQuat Rotation(
				Compiled.Components[3],
				Compiled.Components[4],
				Compiled.Components[5],
				Compiled.Components[6]);
			if (!Rotation.IsNormalized())
			{
				return Fail(
					OutError,
					TEXT("world_spatial_viewport_camera_rotation_invalid"),
					Transform.Subject);
			}
			Client.SetViewLocation(FVector(
				Compiled.Components[0],
				Compiled.Components[1],
				Compiled.Components[2]));
			Client.SetViewRotation(Rotation.Rotator());
			return true;
		}

		bool TryApplyViewportState(
			IUeAgentWorldSpatialSpaceCompiler& Compiler,
			const FWorldSpatialViewportStatePatch& Patch,
			FViewportBinding& Binding,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			if (Patch.CameraTransform.IsSet()
				&& !TryApplyCameraTransform(
					Compiler,
					Patch.CameraTransform.GetValue(),
					*Binding.Client,
					OutError))
			{
				return false;
			}
			if (Patch.FovDegrees.IsSet())
			{
				Binding.Client->ViewFOV = Patch.FovDegrees.GetValue();
			}
			if (Patch.ViewMode.IsSet())
			{
				if (!TryApplyViewModeId(
						Patch.ViewMode.GetValue(),
						*Binding.Client,
						OutError))
				{
					return false;
				}
			}
			if (Patch.Realtime.IsSet())
			{
				ApplyRealtimeState(Patch.Realtime.GetValue(), *Binding.Client);
			}
			if (Patch.GameView.IsSet())
			{
				Binding.Client->SetGameView(Patch.GameView.GetValue());
			}
			if (Patch.ShowFlags.IsSet()
				&& !TryApplyShowFlags(
					*Binding.Client, Patch.ShowFlags.GetValue(), OutError))
			{
				return false;
			}
			if (Patch.Interaction.IsSet())
			{
				const FWorldSpatialViewportInteractionState& Interaction =
					Patch.Interaction.GetValue();
				Binding.Client->EngineShowFlags.SetModeWidgets(
					Interaction.bModeWidgets);
				Binding.Client->EngineShowFlags.SetSelection(
					Interaction.bSelection);
				Binding.Client->EngineShowFlags.SetPivot(
					Interaction.bPivot);
				if (!TryApplyWidgetMode(
						Interaction.WidgetMode,
						*Binding.Client,
						OutError))
				{
					return false;
				}
				for (const TPair<FString, bool>& Flag : {
					TPair<FString, bool>(TEXT("Editor"), Interaction.bEditorPrimitives),
					TPair<FString, bool>(TEXT("Game"), Interaction.bGamePrimitives)})
				{
					const int32 Index = FEngineShowFlags::FindIndexByName(*Flag.Key);
					if (Index < 0)
					{
						return Fail(OutError,
							TEXT("world_spatial_viewport_interaction_flag_missing"),
							Flag.Key);
					}
					Binding.Client->EngineShowFlags.SetSingleFlag(Index, Flag.Value);
				}
			}
			Binding.Client->Invalidate();
			return true;
		}

		FWorldSpatialViewportStatePatch MakePatch(
			const FWorldSpatialViewportState& State)
		{
			FWorldSpatialViewportStatePatch Patch;
			Patch.CameraTransform = State.Camera.CameraTransform;
			Patch.FovDegrees = State.Camera.FovDegrees;
			Patch.ViewMode = State.Camera.ViewMode;
			Patch.Realtime = State.bRealtime;
			Patch.GameView = State.bGameView;
			Patch.ShowFlags = State.ShowFlags;
			Patch.Interaction = State.Interaction;
			return Patch;
		}

		bool TryBoundsForSubjects(
			IUeAgentWorldSpatialNativeEnvironment& Environment,
			const TArray<FString>& SubjectRefs,
			FBox& OutBounds,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			OutBounds = FBox(ForceInit);
			for (const FString& Ref : SubjectRefs)
			{
				UObject* Object = nullptr;
				if (!Environment.TryResolveObject(Ref, Object, OutError))
				{
					return false;
				}
				if (AActor* Actor = Cast<AActor>(Object))
				{
					OutBounds += Actor->GetComponentsBoundingBox(true);
				}
				else if (UPrimitiveComponent* Component =
					Cast<UPrimitiveComponent>(Object))
				{
					OutBounds += Component->Bounds.GetBox();
				}
				else
				{
					return Fail(
						OutError,
						TEXT("world_spatial_viewport_frame_subject_type_invalid"),
						Ref);
				}
			}
			return OutBounds.IsValid
				? true
				: Fail(
					OutError,
					TEXT("world_spatial_viewport_frame_bounds_invalid"),
					FString::Join(SubjectRefs, TEXT(",")));
		}

		bool TryResolveFrameBounds(
			IUeAgentWorldSpatialNativeEnvironment& Environment,
			const FViewportCameraFrameRequestInput& Input,
			const FViewportBinding& Binding,
			FBox& OutBounds,
			TArray<AActor*>& OutActors,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			OutBounds = FBox(ForceInit);
			OutActors.Reset();
			if (!Input.SubjectRefs.IsEmpty())
			{
				for (const FString& Ref : Input.SubjectRefs)
				{
					UObject* Object = nullptr;
					if (!Environment.TryResolveObject(Ref, Object, OutError))
					{
						return false;
					}
					AActor* Actor = Cast<AActor>(Object);
					if (!Actor)
					{
						return Fail(OutError,
							TEXT("world_spatial_viewport_frame_subject_type_invalid"), Ref);
					}
					OutActors.AddUnique(Actor);
					OutBounds += Actor->GetComponentsBoundingBox(true);
				}
			}
			else if (Input.FolderPath.IsSet())
			{
				FString Error;
				if (!UeAgentLevelOps::GetFolderActorBounds(
						Binding.World,
						Input.FolderPath.GetValue(),
						Input.bIncludeChildFolders,
						OutBounds,
						OutActors,
						Error))
				{
					return Fail(OutError,
						TEXT("world_spatial_viewport_frame_folder_invalid"),
						Input.FolderPath.GetValue(), Error);
				}
			}
			else if (Input.bUseCurrentSelection && GEditor)
			{
				if (USelection* Selection = GEditor->GetSelectedActors())
				{
					for (FSelectionIterator It(*Selection); It; ++It)
					{
						AActor* Actor = Cast<AActor>(*It);
						if (Actor && Actor->GetWorld() == Binding.World)
						{
							OutActors.AddUnique(Actor);
							OutBounds += Actor->GetComponentsBoundingBox(true);
						}
					}
				}
			}
			return OutBounds.IsValid && !OutActors.IsEmpty()
				? true
				: Fail(OutError,
					TEXT("world_spatial_viewport_frame_selector_empty"),
					Binding.Identity);
		}

		bool TryFrameCamera(
			IUeAgentWorldSpatialNativeEnvironment& Environment,
			const FViewportCameraFrameRequestInput& Input,
			FViewportBinding& Binding,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			FBox Bounds;
			TArray<AActor*> Actors;
			if (!TryResolveFrameBounds(
					Environment, Input, Binding, Bounds, Actors, OutError))
			{
				return false;
			}
			const FIntPoint Size = Binding.Viewport->GetSizeXY();
			const double Aspect = static_cast<double>(Size.X) / Size.Y;
			const double HorizontalHalf = FMath::DegreesToRadians(
				Binding.Client->ViewFOV * 0.5);
			const double VerticalHalf = FMath::Atan(
				FMath::Tan(HorizontalHalf) / Aspect);
			const double HalfAngle = FMath::Max(
				0.001, FMath::Min(HorizontalHalf, VerticalHalf));
			const double Radius = FMath::Max(1.0, Bounds.GetExtent().Size());
			const double Distance = FMath::Clamp(
				Radius * (1.0 + Input.FramePolicy.PaddingRatio)
					/ FMath::Tan(HalfAngle),
				Input.FramePolicy.MinimumDistanceCm,
				Input.FramePolicy.MaximumDistanceCm);
			FRotator Rotation = Binding.Client->GetViewRotation();
			if (Input.FramePolicy.bLookAt || !Input.FramePolicy.bPreserveRotation)
			{
				const FVector Toward = Bounds.GetCenter()
					- Binding.Client->GetViewLocation();
				if (!Toward.IsNearlyZero())
				{
					Rotation = Toward.Rotation();
				}
			}
			FVector Location = Bounds.GetCenter() - Rotation.Vector() * Distance;
			if (Input.FramePolicy.bCollisionAware && Binding.World)
			{
				FCollisionQueryParams QueryParams(
					SCENE_QUERY_STAT(UeAgentWorldSpatialViewportFrame), false);
				for (AActor* Actor : Actors)
				{
					QueryParams.AddIgnoredActor(Actor);
				}
				FHitResult Hit;
				if (Binding.World->LineTraceSingleByChannel(
						Hit, Bounds.GetCenter(), Location, ECC_Visibility, QueryParams))
				{
					Location = Hit.ImpactPoint + Hit.ImpactNormal * 15.0;
					if (Input.FramePolicy.bAutoFallback
						&& FVector::Distance(Location, Bounds.GetCenter())
							< Input.FramePolicy.MinimumDistanceCm)
					{
						const FVector Directions[] = {
							-FVector::ForwardVector, FVector::ForwardVector,
							-FVector::RightVector, FVector::RightVector,
							FVector(0.0, 0.0, 1.0)};
						for (const FVector& Direction : Directions)
						{
							const FVector Candidate = Bounds.GetCenter() + Direction * Distance;
							FHitResult CandidateHit;
							if (!Binding.World->LineTraceSingleByChannel(
									CandidateHit, Bounds.GetCenter(), Candidate,
									ECC_Visibility, QueryParams))
							{
								Location = Candidate;
								break;
							}
						}
					}
				}
			}
			if (Input.FramePolicy.bLookAt)
			{
				Rotation = (Bounds.GetCenter() - Location).Rotation();
			}
			Binding.Client->SetViewRotation(Rotation);
			Binding.Client->SetViewLocation(Location);
			Binding.Client->Invalidate();
			return true;
		}

		bool TryResolveSelectionActors(
			IUeAgentWorldSpatialNativeEnvironment& Environment,
			const TArray<FString>& ActorRefs,
			TArray<AActor*>& OutActors,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			OutActors.Reset();
			for (const FString& Ref : ActorRefs)
			{
				UObject* Object = nullptr;
				if (!Environment.TryResolveObject(Ref, Object, OutError))
				{
					return false;
				}
				AActor* Actor = Cast<AActor>(Object);
				if (!Actor)
				{
					return Fail(OutError,
						TEXT("world_spatial_viewport_selection_actor_invalid"), Ref);
				}
				OutActors.AddUnique(Actor);
			}
			return !OutActors.IsEmpty()
				? true
				: Fail(OutError,
					TEXT("world_spatial_viewport_selection_actor_set_empty"),
					TEXT("actor_refs"));
		}

		bool TrySelectedActorRefSet(
			IUeAgentWorldSpatialNativeEnvironment& Environment,
			TSet<FString>& OutRefs,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			OutRefs.Reset();
			if (!GEditor || !GEditor->GetSelectedActors())
			{
				return true;
			}
			for (FSelectionIterator It(*GEditor->GetSelectedActors()); It; ++It)
			{
				AActor* Actor = Cast<AActor>(*It);
				if (!Actor)
				{
					continue;
				}
				FString Ref;
				if (!Environment.TryGetExplicitObjectRef(Actor, Ref, OutError))
				{
					return false;
				}
				OutRefs.Add(MoveTemp(Ref));
			}
			return true;
		}

		FString ActorRefSetFingerprint(const TSet<FString>& Refs)
		{
			TArray<FString> Sorted = Refs.Array();
			Sorted.Sort();
			return FString::Join(Sorted, TEXT("\n"));
		}

		TSet<FString> ActorRefSetFromFingerprint(const FString& Fingerprint)
		{
			TArray<FString> Values;
			Fingerprint.ParseIntoArray(Values, TEXT("\n"), true);
			TSet<FString> Result;
			for (FString& Value : Values)
			{
				Result.Add(MoveTemp(Value));
			}
			return Result;
		}

		bool TrySnapshotExplicitSelection(
			const FUeAgentWorldSpatialExecutionChainContract& Contract,
			const EUeAgentWorldSpatialReadPhase Phase,
			const FViewportBinding& Binding,
			IUeAgentWorldSpatialNativeEnvironment& Environment,
			const FUeAgentOperationContext& Context,
			TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			TSet<FString> SelectedRefs;
			if (!TrySelectedActorRefSet(Environment, SelectedRefs, OutError))
			{
				return false;
			}
			const FString Fingerprint = ActorRefSetFingerprint(SelectedRefs);
			FUeAgentCanonicalValuePtr Value;
			if (!TryMakeStringValue(Fingerprint, Context, Value, OutError))
			{
				return false;
			}
			return TryMakeSnapshot(
				Contract,
				Phase,
				Binding.Identity + TEXT("|selection|") + Fingerprint,
				{{TEXT("world-spatial:viewport-selection:selected-actor-refs"),
					TEXT("editor_selection"), Binding.Identity, Binding.Identity,
					TEXT("viewport_selection.selected_actor_refs"),
					TEXT("core.string"), Value}},
				Context,
				OutSnapshot,
				OutError);
		}

		bool TrySnapshotRay(
			const FUeAgentWorldSpatialExecutionChainContract& Contract,
			EUeAgentWorldSpatialReadPhase Phase,
			const FViewportBinding& Binding,
			const FViewportRay& Ray,
			const TOptional<FHitResult>& Hit,
			IUeAgentWorldSpatialNativeEnvironment& Environment,
			const FUeAgentOperationContext& Context,
			TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			TArray<FObservedEntry> Entries;
			FUeAgentCanonicalValuePtr Origin;
			FUeAgentCanonicalValuePtr Direction;
			if (!TryMakeCompiledValue(
					MakeVectorValue(
						Binding.Identity + TEXT(".ray.origin"),
						TEXT("point"),
						TEXT("unreal.world"),
						TEXT("centimeter"),
						Ray.Origin,
						Ray.EvidenceId),
					Context,
					Origin,
					OutError)
				|| !TryMakeCompiledValue(
					MakeVectorValue(
						Binding.Identity + TEXT(".ray.direction"),
						TEXT("direction"),
						TEXT("unreal.world"),
						TEXT("unitless"),
						Ray.Direction,
						Ray.EvidenceId),
					Context,
					Direction,
					OutError))
			{
				return false;
			}
			Entries.Add({TEXT("world-spatial:viewport-ray:origin"), TEXT("viewport_ray"), Binding.Identity, Binding.Identity, TEXT("viewport_ray.origin"), TEXT("world_spatial.compiled_value.v1"), Origin});
			Entries.Add({TEXT("world-spatial:viewport-ray:direction"), TEXT("viewport_ray"), Binding.Identity, Binding.Identity, TEXT("viewport_ray.direction"), TEXT("world_spatial.compiled_value.v1"), Direction});
			FString Fingerprint = FString::Printf(
				TEXT("%s|%.9g|%.9g|%.9g|%.9g|%.9g|%.9g"),
				*Binding.Identity,
				Ray.Origin.X, Ray.Origin.Y, Ray.Origin.Z,
				Ray.Direction.X, Ray.Direction.Y, Ray.Direction.Z);
			if (Hit.IsSet())
			{
				const FHitResult& Result = Hit.GetValue();
				FString ActorRef;
				if (Result.bBlockingHit && Result.GetActor()
					&& !Environment.TryGetExplicitObjectRef(
						Result.GetActor(), ActorRef, OutError))
				{
					return false;
				}
				const FString HitSubject = ActorRef.IsEmpty()
					? Binding.Identity : ActorRef;
				FUeAgentCanonicalValuePtr Blocking;
				FUeAgentCanonicalValuePtr Target;
				FUeAgentCanonicalValuePtr Location;
				FUeAgentCanonicalValuePtr Normal;
				FUeAgentCanonicalValuePtr Distance;
				if (!TryMakeBooleanValue(
						Result.bBlockingHit, Context, Blocking, OutError)
					|| !TryMakeStringValue(ActorRef, Context, Target, OutError)
					|| !TryMakeCompiledValue(
						MakeVectorValue(
							Binding.Identity + TEXT(".hit.location"),
							TEXT("point"),
							TEXT("unreal.world"),
							TEXT("centimeter"),
							Result.Location,
							TEXT("independent-world-trace")),
						Context, Location, OutError)
					|| !TryMakeCompiledValue(
						MakeVectorValue(
							Binding.Identity + TEXT(".hit.normal"),
							TEXT("normal"),
							TEXT("unreal.world"),
							TEXT("unitless"),
							Result.Normal.IsNearlyZero()
								? -Ray.Direction : FVector(Result.Normal),
							TEXT("independent-world-trace")),
						Context, Normal, OutError)
					|| !TryMakeDecimalValue(
						Result.Distance, Context, Distance, OutError))
				{
					return false;
				}
				Entries.Add({TEXT("world-spatial:viewport-hit:blocking"), TEXT("viewport_hit"), Binding.Identity, HitSubject, TEXT("viewport_hit.blocking"), TEXT("core.boolean"), Blocking});
				Entries.Add({TEXT("world-spatial:viewport-hit:target"), TEXT("viewport_hit"), Binding.Identity, HitSubject, TEXT("viewport_hit.actor_ref"), TEXT("core.string"), Target});
				Entries.Add({TEXT("world-spatial:viewport-hit:location"), TEXT("viewport_hit"), Binding.Identity, HitSubject, TEXT("viewport_hit.location"), TEXT("world_spatial.compiled_value.v1"), Location});
				Entries.Add({TEXT("world-spatial:viewport-hit:normal"), TEXT("viewport_hit"), Binding.Identity, HitSubject, TEXT("viewport_hit.normal"), TEXT("world_spatial.compiled_value.v1"), Normal});
				Entries.Add({TEXT("world-spatial:viewport-hit:distance"), TEXT("viewport_hit"), Binding.Identity, HitSubject, TEXT("viewport_hit.distance"), TEXT("core.decimal"), Distance});
				Fingerprint += FString::Printf(
					TEXT("|%d|%s|%.9g"),
					Result.bBlockingHit,
					*ActorRef,
					Result.Distance);
			}
			return TryMakeSnapshot(
				Contract,
				Phase,
				Fingerprint,
				Entries,
				Context,
				OutSnapshot,
				OutError);
		}

		bool TrySelectionChangeType(
			const FString& Mode,
			EGeometrySelectionChangeType& Out,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			if (Mode == TEXT("replace")) { Out = EGeometrySelectionChangeType::Replace; return true; }
			if (Mode == TEXT("add")) { Out = EGeometrySelectionChangeType::Add; return true; }
			if (Mode == TEXT("remove")) { Out = EGeometrySelectionChangeType::Remove; return true; }
			return Fail(
				OutError,
				TEXT("world_spatial_mesh_selection_mode_unsupported"),
				Mode);
		}

		bool TryConfigureElementKind(
			UGeometrySelectionManager& Manager,
			const FString& Kind,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			EGeometryElementType Element = EGeometryElementType::Face;
			UGeometrySelectionManager::EMeshTopologyMode Topology =
				UGeometrySelectionManager::EMeshTopologyMode::Triangle;
			if (Kind == TEXT("vertex")) { Element = EGeometryElementType::Vertex; }
			else if (Kind == TEXT("edge")) { Element = EGeometryElementType::Edge; }
			else if (Kind == TEXT("triangle")) { Element = EGeometryElementType::Face; }
			else if (Kind == TEXT("polygon"))
			{
				Element = EGeometryElementType::Face;
				Topology = UGeometrySelectionManager::EMeshTopologyMode::Polygroup;
			}
			else
			{
				return Fail(
					OutError,
					TEXT("world_spatial_mesh_element_kind_unsupported"),
					Kind);
			}
			Manager.SetMeshSelectionTypeAndMode(Element, Topology, true);
			return true;
		}

		TArray<int64> SelectionIds(const FGeometrySelection& Selection)
		{
			TArray<int64> Result;
			for (const uint64 Encoded : Selection.Selection)
			{
				const FGeoSelectionID Id(Encoded);
				Result.Add(static_cast<int64>(Id.GeometryID));
			}
			Result.Sort();
			return Result;
		}

		bool TryResolveMeshSelection(
			IUeAgentWorldSpatialNativeEnvironment& Environment,
			const FUeAgentOperationContext& Context,
			UGeometrySelectionManager*& OutManager,
			TArray<FUeAgentWorldSpatialMeshSelectionTarget>& OutTargets,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			if (!Environment.TryResolveMeshSelectionSession(
				Context.GetExecutionContext().GetViewportIdentity(),
				Context.GetExecutionContext().GetSelectionIdentities(),
				OutManager,
				OutTargets,
				OutError))
			{
				return false;
			}
			if (!OutManager || OutTargets.IsEmpty()
				|| OutTargets.ContainsByPredicate(
					[](const FUeAgentWorldSpatialMeshSelectionTarget& Target)
					{
						return !Target.IsValid();
					}))
			{
				return Fail(
					OutError,
					TEXT("world_spatial_mesh_selection_session_invalid"),
					Context.GetExecutionContext().GetViewportIdentity());
			}
			UInteractiveToolsContext* ToolsContext =
				OutManager->GetToolsContext();
			if (!ToolsContext || !ToolsContext->ToolManager
				|| !ToolsContext->ToolManager->GetContextQueriesAPI()
				|| !OutManager->GetTransactionsAPI())
			{
				return Fail(
					OutError,
					TEXT("world_spatial_mesh_selection_tools_context_invalid"),
					Context.GetExecutionContext().GetViewportIdentity());
			}
			if (OutManager->GetAnyCurrentTargetsLocked())
			{
				return Fail(
					OutError,
					TEXT("world_spatial_mesh_selection_target_locked"),
					Context.GetExecutionContext().GetViewportIdentity());
			}
			return true;
		}

		bool TryMeshSelectionSnapshot(
			const FUeAgentWorldSpatialExecutionChainContract& Contract,
			const FMeshElementSelectByRayRequestInput& Input,
			EUeAgentWorldSpatialReadPhase Phase,
			IUeAgentWorldSpatialNativeEnvironment& Environment,
			const FUeAgentOperationContext& Context,
			TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			UGeometrySelectionManager* Manager = nullptr;
			TArray<FUeAgentWorldSpatialMeshSelectionTarget> Targets;
			if (!TryResolveMeshSelection(
					Environment, Context, Manager, Targets, OutError))
			{
				return false;
			}
			TArray<FObservedEntry> Entries;
			FString Fingerprint = Input.ElementKind;
			for (const FUeAgentWorldSpatialMeshSelectionTarget& Target : Targets)
			{
				FGeometrySelection Selection;
				const bool bHasSelection =
					Manager->GetSelectionForComponent(Target.Component, Selection);
				const TArray<int64> Ids = bHasSelection
					? SelectionIds(Selection) : TArray<int64>();
				FUeAgentCanonicalValuePtr Value;
				if (!TryMakeIntegerArray(Ids, Context, Value, OutError))
				{
					return false;
				}
				Entries.Add({
					TEXT("world-spatial:mesh-selection:") + Target.ExplicitRef,
					TEXT("mesh_element_selection"),
					Context.GetExecutionContext().GetViewportIdentity(),
					Target.ExplicitRef,
					TEXT("mesh_selection:") + Target.ExplicitRef,
					TEXT("core.integer_array"),
					Value,
				});
				Fingerprint += TEXT("|") + Target.ExplicitRef + TEXT(":");
				for (const int64 Id : Ids)
				{
					Fingerprint += FString::Printf(TEXT("%lld,"), Id);
				}
			}
			return TryMakeSnapshot(
				Contract,
				Phase,
				Fingerprint,
				Entries,
				Context,
				OutSnapshot,
				OutError);
		}

		class FViewportProjectionNativeFamily final
			: public IWorldSpatialNativeFamily
		{
		public:
			explicit FViewportProjectionNativeFamily(
				const FUeAgentWorldSpatialNativeDependencies& InDependencies)
				: Environment(InDependencies.Environment.ToSharedRef())
				, SpaceCompiler(InDependencies.SpaceCompiler.ToSharedRef())
			{
			}

			const FString& GetFamilyId() const override
			{
				static const FString Id = TEXT("viewport_projection");
				return Id;
			}

			bool TryRead(
				const FUeAgentWorldSpatialExecutionChainContract& Contract,
				const FUeAgentWorldSpatialRequest& Request,
				const TOptional<FUeAgentCandidateBinding>& Target,
				EUeAgentWorldSpatialReadPhase Phase,
				const FUeAgentOperationContext& Context,
				TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
				FUeAgentWorldSpatialExecutionError& OutError) override
			{
				if (Request.GetPayload().IsType<FMeshElementSelectByRayRequestInput>())
				{
					return TryMeshSelectionSnapshot(
						Contract,
						Request.GetPayload().Get<FMeshElementSelectByRayRequestInput>(),
						Phase,
						*Environment,
						Context,
						OutSnapshot,
						OutError);
				}

				FViewportBinding Binding;
				if (!TryResolveViewport(*Environment, Context, Binding, OutError))
				{
					return false;
				}
				if (Request.GetPayload().IsType<FViewportPixelScaleReadRequestInput>())
				{
					const FWorldSpatialValue& Sample = Request.GetPayload()
						.Get<FViewportPixelScaleReadRequestInput>().SamplePoint;
					FUeAgentWorldSpatialCompiledValue Point;
					if (!SpaceCompiler->TryConvertToNative(
							Sample,
							TEXT("unreal.world"),
							TEXT("centimeter"),
							Point,
							OutError))
					{
						return false;
					}
					FSceneViewFamilyContext ViewFamily = MakeViewFamily(Binding);
					FSceneView* View = nullptr;
					if (!TryBuildSceneView(Binding, ViewFamily, View, OutError))
					{
						return false;
					}
					const FVector WorldPoint(
						Point.Components[0], Point.Components[1], Point.Components[2]);
					FVector2D Pixel;
					FVector2D NeighborPixel;
					const FVector CameraRight =
						Binding.Client->GetViewRotation().RotateVector(FVector::RightVector);
					if (!View->WorldToPixel(WorldPoint, Pixel)
						|| !View->WorldToPixel(WorldPoint + CameraRight, NeighborPixel))
					{
						return Fail(
							OutError,
							TEXT("world_spatial_viewport_pixel_scale_projection_failed"),
							Sample.Subject);
					}
					const double PixelDistance = FVector2D::Distance(Pixel, NeighborPixel);
					if (PixelDistance <= UE_SMALL_NUMBER)
					{
						return Fail(
							OutError,
							TEXT("world_spatial_viewport_pixel_scale_degenerate"),
							Sample.Subject);
					}
					FUeAgentCanonicalValuePtr SampleValue;
					FUeAgentCanonicalValuePtr Scale;
					FUeAgentCanonicalValuePtr Camera;
					FUeAgentCanonicalValuePtr EvidenceIds;
					if (!TryMakeCompiledValue(Point, Context, SampleValue, OutError)
						|| !TryMakeDecimalValue(
							1.0 / PixelDistance, Context, Scale, OutError)
						|| !TryMakeViewportCameraValue(
							Binding, Context, Camera, OutError)
						|| !TryMakeStringArray(
							{
								Point.EvidenceId,
								TEXT("camera-projection-neighbor-sample:")
									+ Binding.Identity,
							},
							Context,
							EvidenceIds,
							OutError))
					{
						return false;
					}
					return TryMakeSnapshot(
						Contract,
						Phase,
						FString::Printf(
							TEXT("%s|%.9g|%s"),
							*Sample.Subject,
							1.0 / PixelDistance,
							*CameraFingerprint(Binding)),
						{
							{TEXT("world-spatial:pixel-scale:point"), TEXT("viewport_projection"), Binding.Identity, Sample.Subject, TEXT("viewport_pixel_scale.sample_point"), TEXT("world_spatial.compiled_value.v1"), SampleValue},
							{TEXT("world-spatial:pixel-scale:value"), TEXT("viewport_projection"), Binding.Identity, Sample.Subject, TEXT("viewport_pixel_scale.world_units_per_pixel"), TEXT("core.decimal"), Scale},
							{TEXT("world-spatial:pixel-scale:camera"), TEXT("viewport_projection"), Binding.Identity, Binding.Identity, TEXT("viewport_pixel_scale.camera"), TEXT("world_spatial.viewport_camera_state.v1"), Camera},
							{TEXT("world-spatial:pixel-scale:evidence"), TEXT("viewport_projection"), Binding.Identity, Sample.Subject, TEXT("viewport_pixel_scale.evidence_ids"), TEXT("core.string_array"), EvidenceIds},
						},
						Context,
						OutSnapshot,
						OutError);
				}

				const FWorldSpatialValue* ScreenPoint = nullptr;
				TOptional<FWorldSpatialTraceQuery> TraceQuery;
				bool bSelectionReadback = false;
				bool bPickRequiresHit = false;
				if (Request.GetPayload().IsType<FViewportRayDeprojectRequestInput>())
				{
					ScreenPoint = &Request.GetPayload()
						.Get<FViewportRayDeprojectRequestInput>().ScreenPoint;
				}
				else if (Request.GetPayload().IsType<FViewportRayTraceRequestInput>())
				{
					const auto& Input = Request.GetPayload()
						.Get<FViewportRayTraceRequestInput>();
					ScreenPoint = &Input.ScreenPoint;
					TraceQuery = Input.TraceQuery;
				}
				else if (Request.GetPayload().IsType<FViewportActorPickRequestInput>())
				{
					const auto& Input = Request.GetPayload()
						.Get<FViewportActorPickRequestInput>();
					ScreenPoint = &Input.ScreenPoint;
					TraceQuery = Input.TraceQuery;
					bPickRequiresHit = !Input.bAllowNoHit;
				}
				else if (Request.GetPayload().IsType<FViewportActorSelectRequestInput>())
				{
					const auto& Input = Request.GetPayload()
						.Get<FViewportActorSelectRequestInput>();
					if (!Input.ActorRefs.IsEmpty())
					{
						return TrySnapshotExplicitSelection(
							Contract, Phase, Binding, *Environment, Context,
							OutSnapshot, OutError);
					}
					if (!Input.ScreenPoint.IsSet())
					{
						return Fail(OutError,
							TEXT("world_spatial_viewport_actor_selection_selector_missing"),
							Binding.Identity);
					}
					ScreenPoint = &Input.ScreenPoint.GetValue();
					TraceQuery = Input.TraceQuery.IsSet()
						? Input.TraceQuery.GetValue() : DefaultTraceQuery();
					bSelectionReadback = true;
				}
				if (!ScreenPoint)
				{
					return Fail(
						OutError,
						TEXT("world_spatial_viewport_projection_request_mismatch"),
						Contract.GetCapabilityId());
				}
				FViewportRay Ray;
				if (!TryDeproject(
						*SpaceCompiler, *ScreenPoint, Binding, Ray, OutError))
				{
					return false;
				}
				TOptional<FHitResult> Hit;
				if (TraceQuery.IsSet())
				{
					FHitResult Result;
					if (!TryTrace(
							*Environment,
							Binding,
							Ray,
							TraceQuery.GetValue(),
							Result,
							OutError))
					{
						return false;
					}
					Hit = Result;
				}
				if (bPickRequiresHit
					&& Hit.IsSet()
					&& (!Hit->bBlockingHit || !Hit->GetActor()))
				{
					return Fail(
						OutError,
						TEXT("world_spatial_viewport_actor_pick_hit_missing"),
						Binding.Identity);
				}
				if (!bSelectionReadback)
				{
					return TrySnapshotRay(
						Contract, Phase, Binding, Ray, Hit, *Environment,
						Context, OutSnapshot, OutError);
				}
				if (!Hit.IsSet() || !Hit->bBlockingHit || !Hit->GetActor())
				{
					return Fail(
						OutError,
						TEXT("world_spatial_viewport_actor_selection_hit_missing"),
						Binding.Identity);
				}
				FString ActorRef;
				if (!Environment->TryGetExplicitObjectRef(
						Hit->GetActor(), ActorRef, OutError))
				{
					return false;
				}
				FUeAgentCanonicalValuePtr Ref;
				FUeAgentCanonicalValuePtr Selected;
				const bool bSelected = GEditor && GEditor->GetSelectedActors()
					&& GEditor->GetSelectedActors()->IsSelected(Hit->GetActor());
				if (!TryMakeStringValue(ActorRef, Context, Ref, OutError)
					|| !TryMakeBooleanValue(bSelected, Context, Selected, OutError))
				{
					return false;
				}
				return TryMakeSnapshot(
					Contract,
					Phase,
					FString::Printf(TEXT("%s|%s|%d"), *Binding.Identity, *ActorRef, bSelected),
					{
						{TEXT("world-spatial:viewport-selection:hit"), TEXT("editor_selection"), Binding.Identity, ActorRef, TEXT("viewport_selection.hit_actor_ref"), TEXT("core.string"), Ref},
						{TEXT("world-spatial:viewport-selection:selected"), TEXT("editor_selection"), Binding.Identity, ActorRef, TEXT("viewport_selection.hit_actor_selected"), TEXT("core.boolean"), Selected},
					},
					Context,
					OutSnapshot,
					OutError);
			}

			bool TryMutate(
				const FUeAgentWorldSpatialExecutionChainContract& Contract,
				const FUeAgentWorldSpatialRequest& Request,
				const TArray<FUeAgentOperationTargetBinding>& Targets,
				const FUeAgentOperationContext& Context,
				FUeAgentWorldSpatialMutationResult& OutResult,
				FUeAgentWorldSpatialExecutionError& OutError) override
			{
				FViewportBinding Binding;
				if (!TryResolveViewport(*Environment, Context, Binding, OutError))
				{
					return false;
				}
				if (Request.GetPayload().IsType<FViewportActorSelectRequestInput>())
				{
					const auto& Input = Request.GetPayload()
						.Get<FViewportActorSelectRequestInput>();
					TArray<AActor*> Actors;
					if (!Input.ActorRefs.IsEmpty())
					{
						if (!TryResolveSelectionActors(
								*Environment, Input.ActorRefs, Actors, OutError))
						{
							return false;
						}
					}
					else
					{
						FViewportRay Ray;
						FHitResult Hit;
						const FWorldSpatialTraceQuery Query = Input.TraceQuery.IsSet()
							? Input.TraceQuery.GetValue() : DefaultTraceQuery();
						if (!Input.ScreenPoint.IsSet()
							|| !TryDeproject(
								*SpaceCompiler, Input.ScreenPoint.GetValue(), Binding, Ray, OutError)
							|| !TryTrace(
								*Environment, Binding, Ray, Query, Hit, OutError)
							|| !Hit.bBlockingHit || !Hit.GetActor())
						{
							return OutError.IsValid()
								? false
								: Fail(
									OutError,
									TEXT("world_spatial_viewport_actor_selection_hit_missing"),
									Binding.Identity);
						}
						Actors.Add(Hit.GetActor());
					}
					if (!GEditor)
					{
						return Fail(OutError,
							TEXT("world_spatial_viewport_actor_selection_editor_missing"),
							Binding.Identity);
					}
					if (Input.SelectionMode == TEXT("replace"))
					{
						GEditor->SelectNone(false, true, false);
					}
					for (AActor* Actor : Actors)
					{
						const bool bWasSelected = GEditor->GetSelectedActors()
							&& GEditor->GetSelectedActors()->IsSelected(Actor);
						if (Input.SelectionMode == TEXT("replace")
							|| Input.SelectionMode == TEXT("add"))
						{
							GEditor->SelectActor(Actor, true, true, true);
						}
						else if (Input.SelectionMode == TEXT("remove"))
						{
							GEditor->SelectActor(Actor, false, true, true);
						}
						else if (Input.SelectionMode == TEXT("toggle"))
						{
							GEditor->SelectActor(Actor, !bWasSelected, true, true);
						}
						else
						{
							return Fail(
								OutError,
								TEXT("world_spatial_viewport_actor_selection_mode_unsupported"),
								Input.SelectionMode);
						}
					}
					OutResult = FUeAgentWorldSpatialMutationResult(
						TEXT("world-spatial:") + Context.GetRequestId(),
						TEXT("explicit-editor-selection-write:") + Binding.Identity);
					return OutResult.IsValid();
				}
				if (Request.GetPayload().IsType<FMeshElementSelectByRayRequestInput>())
				{
					const auto& Input = Request.GetPayload()
						.Get<FMeshElementSelectByRayRequestInput>();
					UGeometrySelectionManager* Manager = nullptr;
					TArray<FUeAgentWorldSpatialMeshSelectionTarget> SelectionTargets;
					FViewportRay Ray;
					EGeometrySelectionChangeType Change =
						EGeometrySelectionChangeType::Add;
					if (!TryResolveMeshSelection(
							*Environment, Context, Manager, SelectionTargets, OutError)
						|| !TryConfigureElementKind(
							*Manager, Input.ElementKind, OutError)
						|| !TrySelectionChangeType(
							Input.SelectionMode, Change, OutError)
						|| !TryDeproject(
							*SpaceCompiler, Input.ScreenPoint, Binding, Ray, OutError))
					{
						return false;
					}
					if (Input.TraceQuery.IsSet())
					{
						FHitResult FilterHit;
						if (!TryTrace(
								*Environment,
								Binding,
								Ray,
								Input.TraceQuery.GetValue(),
								FilterHit,
								OutError))
						{
							return false;
						}
						if (!FilterHit.bBlockingHit)
						{
							return Fail(
								OutError,
								TEXT("world_spatial_mesh_selection_ray_missed"),
								Binding.Identity);
						}
						const bool bHitSelectionTarget =
							SelectionTargets.ContainsByPredicate(
								[&FilterHit](
									const FUeAgentWorldSpatialMeshSelectionTarget& Target)
								{
									return FilterHit.GetComponent()
											== Target.Component
										|| (FilterHit.GetActor()
											&& Target.Component->GetOwner()
												== FilterHit.GetActor());
								});
						if (!bHitSelectionTarget)
						{
							return Fail(
								OutError,
								TEXT("world_spatial_mesh_selection_ray_occluded"),
								Binding.Identity);
						}
					}
					FGeometrySelectionUpdateConfig Config;
					Config.ChangeType = Change;
					const FRay3d WorldRay(Ray.Origin, Ray.Direction);
					FInputRayHit RayHit;
					if (!Manager->RayHitTest(WorldRay, RayHit))
					{
						return Fail(
							OutError,
							TEXT("world_spatial_mesh_selection_ray_missed"),
							Binding.Identity);
					}
					if (Input.TraceQuery.IsSet()
						&& RayHit.HitDepth > Input.TraceQuery->MaxDistanceCm)
					{
						return Fail(
							OutError,
							TEXT("world_spatial_mesh_selection_beyond_max_distance"),
							Binding.Identity);
					}
					FGeometrySelectionUpdateResult Update;
					Manager->UpdateSelectionViaRaycast(
						WorldRay,
						Config,
						Update);
					OutResult = FUeAgentWorldSpatialMutationResult(
						TEXT("world-spatial:") + Context.GetRequestId(),
						TEXT("explicit-modeling-selection-write:") + Binding.Identity);
					return OutResult.IsValid();
				}
				return Fail(
					OutError,
					TEXT("world_spatial_viewport_projection_mutation_mismatch"),
					Contract.GetCapabilityId());
			}

			bool TrySettle(
				const FUeAgentWorldSpatialExecutionChainContract& Contract,
				const FUeAgentWorldSpatialMutationResult& Mutation,
				const FUeAgentOperationContext& Context,
				FUeAgentWorldSpatialExecutionError& OutError) override
			{
				FViewportBinding Binding;
				return Mutation.IsValid()
					&& TryResolveViewport(*Environment, Context, Binding, OutError)
						? true
						: OutError.IsValid()
							? false
							: Fail(
								OutError,
								TEXT("world_spatial_viewport_projection_not_settled"),
								Contract.GetSettleBarrier());
			}

			bool TryCompare(
				const FUeAgentWorldSpatialExecutionChainContract& Contract,
				const FUeAgentWorldSpatialRequest& Request,
				const FUeAgentCanonicalSnapshot& Before,
				const FUeAgentCanonicalSnapshot& After,
				const FUeAgentOperationContext& Context,
				FVerificationOutcome& OutOutcome,
				FUeAgentWorldSpatialExecutionError& OutError) override
			{
				bool bMatches = false;
				if (Request.GetPayload().IsType<FViewportActorSelectRequestInput>())
				{
					const FViewportActorSelectRequestInput& Input = Request.GetPayload()
						.Get<FViewportActorSelectRequestInput>();
					const FString& Mode = Input.SelectionMode;
					if (!Input.ActorRefs.IsEmpty())
					{
						FString BeforeFingerprint;
						FString AfterFingerprint;
						if (!TryReadStringValue(Before,
								TEXT("viewport_selection.selected_actor_refs"),
								BeforeFingerprint, OutError)
							|| !TryReadStringValue(After,
								TEXT("viewport_selection.selected_actor_refs"),
								AfterFingerprint, OutError))
						{
							return false;
						}
						TSet<FString> Expected =
							ActorRefSetFromFingerprint(BeforeFingerprint);
						TSet<FString> Targets;
						for (const FString& Ref : Input.ActorRefs)
						{
							Targets.Add(Ref);
						}
						if (Mode == TEXT("replace"))
						{
							Expected = Targets;
						}
						else if (Mode == TEXT("add"))
						{
							for (const FString& Ref : Targets) { Expected.Add(Ref); }
						}
						else if (Mode == TEXT("remove"))
						{
							for (const FString& Ref : Targets) { Expected.Remove(Ref); }
						}
						else if (Mode == TEXT("toggle"))
						{
							for (const FString& Ref : Targets)
							{
								if (Expected.Contains(Ref)) { Expected.Remove(Ref); }
								else { Expected.Add(Ref); }
							}
						}
						bMatches = ActorRefSetFingerprint(Expected) == AfterFingerprint;
					}
					else
					{
					bool bBefore = false;
					bool bAfter = false;
					FString BeforeRef;
					FString AfterRef;
					if (!TryReadBooleanValue(
							Before, TEXT("viewport_selection.hit_actor_selected"),
							bBefore, OutError)
						|| !TryReadBooleanValue(
							After, TEXT("viewport_selection.hit_actor_selected"),
							bAfter, OutError)
						|| !TryReadStringValue(
							Before, TEXT("viewport_selection.hit_actor_ref"),
							BeforeRef, OutError)
						|| !TryReadStringValue(
							After, TEXT("viewport_selection.hit_actor_ref"),
							AfterRef, OutError))
					{
						return false;
					}
					bMatches = BeforeRef == AfterRef
						&& ((Mode == TEXT("replace") || Mode == TEXT("add"))
							? bAfter
							: Mode == TEXT("remove")
								? !bAfter
								: Mode == TEXT("toggle") && bAfter != bBefore);
					}
				}
				else if (Request.GetPayload().IsType<FMeshElementSelectByRayRequestInput>())
				{
					const auto& Input = Request.GetPayload()
						.Get<FMeshElementSelectByRayRequestInput>();
					UGeometrySelectionManager* Manager = nullptr;
					TArray<FUeAgentWorldSpatialMeshSelectionTarget> SelectionTargets;
					if (!TryResolveMeshSelection(
							*Environment, Context, Manager, SelectionTargets, OutError))
					{
						return false;
					}
					int32 BeforeCount = 0;
					int32 AfterCount = 0;
					for (const FUeAgentWorldSpatialMeshSelectionTarget& Target
						: SelectionTargets)
					{
						TArray<int64> BeforeIds;
						TArray<int64> AfterIds;
						const FString Role =
							TEXT("mesh_selection:") + Target.ExplicitRef;
						if (!TryReadIntegerArray(Before, Role, BeforeIds, OutError)
							|| !TryReadIntegerArray(After, Role, AfterIds, OutError))
						{
							return false;
						}
						BeforeCount += BeforeIds.Num();
						AfterCount += AfterIds.Num();
					}
					bMatches = Input.SelectionMode == TEXT("replace")
						? AfterCount == 1
						: Input.SelectionMode == TEXT("add")
							? AfterCount >= BeforeCount && AfterCount > 0
							: Input.SelectionMode == TEXT("remove")
								&& AfterCount <= BeforeCount;
				}
				else
				{
					return Fail(
						OutError,
						TEXT("world_spatial_viewport_projection_compare_mismatch"),
						Contract.GetCapabilityId());
				}
				SetComparisonOutcome(
					Contract,
					bMatches,
					TEXT("same-explicit-viewport-independent-selection-readback"),
					OutOutcome);
				return true;
			}

		private:
			TSharedRef<IUeAgentWorldSpatialNativeEnvironment> Environment;
			TSharedRef<IUeAgentWorldSpatialSpaceCompiler> SpaceCompiler;
		};

		class FViewportSessionStateNativeFamily final
			: public IWorldSpatialNativeFamily
		{
		public:
			explicit FViewportSessionStateNativeFamily(
				const FUeAgentWorldSpatialNativeDependencies& InDependencies)
				: Environment(InDependencies.Environment.ToSharedRef())
				, SpaceCompiler(InDependencies.SpaceCompiler.ToSharedRef())
				, RevisionSource(InDependencies.RevisionSource.ToSharedRef())
			{
			}

			const FString& GetFamilyId() const override
			{
				static const FString Id = TEXT("viewport_session_state");
				return Id;
			}

			bool TryRead(
				const FUeAgentWorldSpatialExecutionChainContract& Contract,
				const FUeAgentWorldSpatialRequest& Request,
				const TOptional<FUeAgentCandidateBinding>& Target,
				EUeAgentWorldSpatialReadPhase Phase,
				const FUeAgentOperationContext& Context,
				TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
				FUeAgentWorldSpatialExecutionError& OutError) override
			{
				if (Request.GetPayload().IsType<FViewportInstanceListRequestInput>())
				{
					TArray<FString> Identities;
					Environment->GetExplicitViewportIdentities(Identities);
					Identities.Sort();
					if (Identities.IsEmpty())
					{
						return Fail(
							OutError,
							TEXT("world_spatial_viewport_instance_catalog_empty"),
							TEXT("editor.viewport.catalog"));
					}
					TArray<FUeAgentCanonicalValueRef> InstanceItems;
					TArray<FString> Fingerprints;
					for (const FString& Identity : Identities)
					{
						FViewportBinding InstanceBinding;
						FUeAgentCanonicalValuePtr Ref;
						FUeAgentCanonicalValuePtr Type;
						FUeAgentCanonicalValuePtr Camera;
						FUeAgentCanonicalValuePtr Instance;
						if (!TryResolveViewportIdentityForCatalog(
								*Environment,
								Identity,
								InstanceBinding,
								OutError)
							|| !TryMakeStringValue(
								Identity, Context, Ref, OutError)
							|| !TryMakeStringValue(
								ViewportTypeToId(
									InstanceBinding.Client->ViewportType),
								Context,
								Type,
								OutError)
							|| !TryMakeViewportCameraValue(
								InstanceBinding, Context, Camera, OutError)
							|| !TryMakeObject(
								{
									{TEXT("viewport_ref"), Ref.ToSharedRef()},
									{TEXT("viewport_type"), Type.ToSharedRef()},
									{TEXT("camera"), Camera.ToSharedRef()},
								},
								Context,
								Instance,
								OutError))
						{
							return false;
						}
						InstanceItems.Add(Instance.ToSharedRef());
						Fingerprints.Add(CameraFingerprint(InstanceBinding));
					}
					FUeAgentCanonicalValuePtr Instances;
					if (!TryMakeArray(
							InstanceItems, Context, Instances, OutError))
					{
						return false;
					}
					return TryMakeSnapshot(
						Contract,
						Phase,
						FString::Join(Fingerprints, TEXT("|")),
						{{TEXT("world-spatial:viewport:instances"),
							TEXT("viewport_catalog"),
							TEXT("editor"),
							TEXT("explicit_viewports"),
							TEXT("viewport_instances.catalog"),
							TEXT("world_spatial.viewport_instance_array.v1"),
							Instances}},
						Context,
						OutSnapshot,
						OutError);
				}

				FViewportBinding Binding;
				if (!TryResolveViewport(*Environment, Context, Binding, OutError))
				{
					return false;
				}
				if (Request.GetPayload().IsType<FViewportWarningListRequestInput>())
				{
					const FViewportWarningListRequestInput& Input = Request.GetPayload()
						.Get<FViewportWarningListRequestInput>();
					const TOptional<FString>& Ref = Input.ViewportRef;
					if (Ref.IsSet() && Ref.GetValue() != Binding.Identity)
					{
						return Fail(
							OutError,
							TEXT("world_spatial_viewport_warning_identity_mismatch"),
							Ref.GetValue());
					}
					FUeAgentCanonicalValuePtr Warnings;
					if (!TryMakeViewportWarnings(
							Binding.World, Input.bIncludeSuppressed,
							Context, Warnings, OutError))
					{
						return false;
					}
					return TryMakeSnapshot(
						Contract, Phase,
						Binding.Identity + TEXT("|warnings|")
							+ (Input.bIncludeSuppressed ? TEXT("all") : TEXT("displayed")),
						{{TEXT("world-spatial:viewport:warnings"), TEXT("viewport"), Binding.Identity, Binding.Identity, TEXT("viewport_warnings"), TEXT("world_spatial.viewport_warning_array.v1"), Warnings}},
						Context, OutSnapshot, OutError);
				}
				if (Request.GetPayload().IsType<FViewportViewModeListRequestInput>())
				{
					FUeAgentCanonicalValuePtr Modes;
					FUeAgentCanonicalValuePtr Active;
					if (!TryMakeViewModeCatalog(Context, Modes, OutError)
						|| !TryMakeStringValue(
							ViewportViewModeId(*Binding.Client),
							Context, Active, OutError))
					{
						return false;
					}
					return TryMakeSnapshot(
						Contract,
						Phase,
						Binding.Identity + TEXT("|view-modes|")
							+ ViewportViewModeId(*Binding.Client),
						{
							{TEXT("world-spatial:viewport:modes"), TEXT("viewport"), Binding.Identity, Binding.Identity, TEXT("viewport_modes.catalog"), TEXT("world_spatial.viewport_view_mode_array.v1"), Modes},
							{TEXT("world-spatial:viewport:active-mode"), TEXT("viewport"), Binding.Identity, Binding.Identity, TEXT("viewport_modes.active"), TEXT("core.string"), Active},
						},
						Context, OutSnapshot, OutError);
				}
				return TryMakeViewportStateSnapshot(
					Contract,
					Phase,
					Binding,
					*RevisionSource,
					Context,
					OutSnapshot,
					OutError);
			}

			bool TryMutate(
				const FUeAgentWorldSpatialExecutionChainContract& Contract,
				const FUeAgentWorldSpatialRequest& Request,
				const TArray<FUeAgentOperationTargetBinding>& Targets,
				const FUeAgentOperationContext& Context,
				FUeAgentWorldSpatialMutationResult& OutResult,
				FUeAgentWorldSpatialExecutionError& OutError) override
			{
				FViewportBinding Binding;
				if (!TryResolveViewport(*Environment, Context, Binding, OutError))
				{
					return false;
				}
				FString Evidence;
				if (Request.GetPayload().IsType<FViewportCameraFrameRequestInput>())
				{
					if (!TryFrameCamera(
							*Environment,
							Request.GetPayload().Get<FViewportCameraFrameRequestInput>(),
							Binding,
							OutError))
					{
						return false;
					}
					Evidence = TEXT("same-viewport-camera-frame:") + Binding.Identity;
				}
				else if (Request.GetPayload().IsType<FViewportStateRestoreRequestInput>())
				{
					const FWorldSpatialViewportState& State = Request.GetPayload()
						.Get<FViewportStateRestoreRequestInput>().Snapshot;
					if (State.ViewportRef != Binding.Identity
						|| State.ObservedRevision.SourceId != Binding.Identity)
					{
						return Fail(
							OutError,
							TEXT("world_spatial_viewport_restore_identity_mismatch"),
							State.ViewportRef,
							Binding.Identity);
					}
					if (!TryApplyViewportState(
							*SpaceCompiler, MakePatch(State), Binding, OutError))
					{
						return false;
					}
					Evidence = TEXT("same-viewport-state-restore:") + Binding.Identity;
				}
				else if (Request.GetPayload().IsType<FViewportStateUpdateRequestInput>())
				{
					if (!TryApplyViewportState(
							*SpaceCompiler,
							Request.GetPayload().Get<FViewportStateUpdateRequestInput>().StatePatch,
							Binding,
							OutError))
					{
						return false;
					}
					Evidence = TEXT("same-viewport-state-update:") + Binding.Identity;
				}
				else
				{
					return Fail(
						OutError,
						TEXT("world_spatial_viewport_session_mutation_mismatch"),
						Contract.GetCapabilityId());
				}
				OutResult = FUeAgentWorldSpatialMutationResult(
					TEXT("world-spatial:") + Context.GetRequestId(), Evidence);
				return OutResult.IsValid();
			}

			bool TrySettle(
				const FUeAgentWorldSpatialExecutionChainContract& Contract,
				const FUeAgentWorldSpatialMutationResult& Mutation,
				const FUeAgentOperationContext& Context,
				FUeAgentWorldSpatialExecutionError& OutError) override
			{
				FViewportBinding Binding;
				return Mutation.IsValid()
					&& TryResolveViewport(*Environment, Context, Binding, OutError)
					&& MakeCameraTransform(Binding).IsValid()
						? true
						: OutError.IsValid()
							? false
							: Fail(
								OutError,
								TEXT("world_spatial_viewport_session_not_settled"),
								Contract.GetSettleBarrier());
			}

			bool TryCompare(
				const FUeAgentWorldSpatialExecutionChainContract& Contract,
				const FUeAgentWorldSpatialRequest& Request,
				const FUeAgentCanonicalSnapshot& Before,
				const FUeAgentCanonicalSnapshot& After,
				const FUeAgentOperationContext& Context,
				FVerificationOutcome& OutOutcome,
				FUeAgentWorldSpatialExecutionError& OutError) override
			{
				FViewportBinding Binding;
				if (!TryResolveViewport(*Environment, Context, Binding, OutError))
				{
					return false;
				}
				bool bMatches = false;
				FString ComparisonEvidence =
					TEXT("same-explicit-viewport-independent-state-readback");
				if (Request.GetPayload().IsType<FViewportCameraFrameRequestInput>())
				{
					const auto& Input = Request.GetPayload()
						.Get<FViewportCameraFrameRequestInput>();
					FBox Bounds;
					TArray<AActor*> Actors;
					FSceneViewFamilyContext ViewFamily = MakeViewFamily(Binding);
					FSceneView* View = nullptr;
					if (!TryResolveFrameBounds(
							*Environment, Input, Binding, Bounds, Actors, OutError)
						|| !TryBuildSceneView(Binding, ViewFamily, View, OutError))
					{
						return false;
					}
					const FIntPoint Size = Binding.Viewport->GetSizeXY();
					const double MarginX = Input.FramePolicy.PaddingRatio * Size.X * 0.5;
					const double MarginY = Input.FramePolicy.PaddingRatio * Size.Y * 0.5;
					bMatches = true;
					for (int32 Corner = 0; Corner < 8; ++Corner)
					{
						const FVector Point(
							(Corner & 1) ? Bounds.Max.X : Bounds.Min.X,
							(Corner & 2) ? Bounds.Max.Y : Bounds.Min.Y,
							(Corner & 4) ? Bounds.Max.Z : Bounds.Min.Z);
						FVector2D Pixel;
						bMatches &= View->WorldToPixel(Point, Pixel)
							&& Pixel.X >= MarginX && Pixel.X <= Size.X - MarginX
							&& Pixel.Y >= MarginY && Pixel.Y <= Size.Y - MarginY;
					}
				}
				else
				{
					FWorldSpatialViewportStatePatch Expected;
					if (Request.GetPayload().IsType<FViewportStateRestoreRequestInput>())
					{
						Expected = MakePatch(Request.GetPayload()
							.Get<FViewportStateRestoreRequestInput>().Snapshot);
					}
					else if (Request.GetPayload().IsType<FViewportStateUpdateRequestInput>())
					{
						Expected = Request.GetPayload()
							.Get<FViewportStateUpdateRequestInput>().StatePatch;
					}
					else
					{
						return Fail(
							OutError,
							TEXT("world_spatial_viewport_session_compare_mismatch"),
							Contract.GetCapabilityId());
					}
					bMatches = true;
					if (Expected.CameraTransform.IsSet())
					{
						FUeAgentWorldSpatialCompiledValue ExpectedCamera;
						FUeAgentWorldSpatialCompiledValue ActualCamera;
						if (!SpaceCompiler->TryConvertToNative(
								Expected.CameraTransform.GetValue(),
								TEXT("unreal.world"),
								TEXT("centimeter"),
								ExpectedCamera,
								OutError)
							|| !TryReadCompiledValue(
								After,
								TEXT("viewport_state.camera_transform"),
								ActualCamera,
								OutError))
						{
							return false;
						}
						bMatches &= NearlyEqualComponents(
							ExpectedCamera.Components, ActualCamera.Components, 0.001);
					}
					if (Expected.FovDegrees.IsSet())
					{
						double Actual = 0.0;
						if (!TryReadDecimalValue(
								After, TEXT("viewport_state.fov"), Actual, OutError))
						{
							return false;
						}
						bMatches &= FMath::IsNearlyEqual(
							Expected.FovDegrees.GetValue(), Actual, 0.001);
					}
					if (Expected.ViewMode.IsSet())
					{
						FString Actual;
						if (!TryReadStringValue(
								After, TEXT("viewport_state.view_mode"), Actual, OutError))
						{
							return false;
						}
						bMatches &= Expected.ViewMode.GetValue() == Actual;
					}
					if (Expected.Realtime.IsSet())
					{
						bool Actual = false;
						if (!TryReadBooleanValue(
								After, TEXT("viewport_state.realtime"), Actual, OutError))
						{
							return false;
						}
						bMatches &= Expected.Realtime.GetValue() == Actual;
					}
					if (Expected.GameView.IsSet())
					{
						bool Actual = false;
						if (!TryReadBooleanValue(
								After, TEXT("viewport_state.game_view"), Actual, OutError))
						{
							return false;
						}
						bMatches &= Expected.GameView.GetValue() == Actual;
					}
					if (Expected.ShowFlags.IsSet())
					{
						const FUeAgentStateEntry* Entry = FindEntryByRole(
							After, TEXT("viewport_state.show_flags"));
						if (!Entry || Entry->GetValue()->GetKind()
							!= EUeAgentValueKind::Array)
						{
							return Fail(
								OutError,
								TEXT("world_spatial_viewport_show_flags_readback_invalid"),
								Binding.Identity);
						}
						TArray<FString> Actual;
						for (const FUeAgentCanonicalValueRef& Item
							: Entry->GetValue()->GetArrayItems())
						{
							Actual.Add(Item->GetText());
						}
						Actual.Sort();
						TArray<FString> Wanted = Expected.ShowFlags.GetValue();
						Wanted.Sort();
						bMatches &= Actual == Wanted;
					}
					if (Expected.Interaction.IsSet())
					{
						const FWorldSpatialViewportInteractionState& Wanted =
							Expected.Interaction.GetValue();
						bool Actual = false;
						FString ActualWidgetMode;
						if (!TryReadBooleanValue(After,
								TEXT("viewport_state.interaction.mode_widgets"), Actual, OutError))
						{
							return false;
						}
						bMatches &= Actual == Wanted.bModeWidgets;
						if (!TryReadBooleanValue(After,
								TEXT("viewport_state.interaction.selection"), Actual, OutError))
						{
							return false;
						}
						bMatches &= Actual == Wanted.bSelection;
						if (!TryReadBooleanValue(After,
								TEXT("viewport_state.interaction.pivot"), Actual, OutError))
						{
							return false;
						}
						bMatches &= Actual == Wanted.bPivot;
						if (!TryReadBooleanValue(After,
								TEXT("viewport_state.interaction.editor_primitives"), Actual, OutError))
						{
							return false;
						}
						bMatches &= Actual == Wanted.bEditorPrimitives;
						if (!TryReadBooleanValue(After,
								TEXT("viewport_state.interaction.game_primitives"), Actual, OutError))
						{
							return false;
						}
						bMatches &= Actual == Wanted.bGamePrimitives;
						if (!TryReadStringValue(After,
								TEXT("viewport_state.interaction.widget_mode"),
								ActualWidgetMode, OutError))
						{
							return false;
						}
						bMatches &= ActualWidgetMode == Wanted.WidgetMode;
					}
				}
				if (!bMatches
					&& !Request.GetPayload().IsType<FViewportCameraFrameRequestInput>())
				{
					FViewportStateObservation Actual;
					FUeAgentWorldSpatialExecutionError DiagnosticError;
					if (TryObserveViewportState(
							*RevisionSource, Binding, Actual, DiagnosticError))
					{
						ComparisonEvidence += FString::Printf(
							TEXT(":actual=fov=%.6g,mode=%s,realtime=%d,game_view=%d,flags=%s,interaction=%d/%d/%d/%d/%d/%s"),
							Actual.FovDegrees,
							*Actual.ViewMode,
							Actual.bRealtime,
							Actual.bGameView,
							*FString::Join(Actual.ShowFlags, TEXT(",")),
							Actual.Interaction.bModeWidgets,
							Actual.Interaction.bSelection,
							Actual.Interaction.bPivot,
							Actual.Interaction.bEditorPrimitives,
							Actual.Interaction.bGamePrimitives,
							*Actual.Interaction.WidgetMode);
					}
				}
				SetComparisonOutcome(
					Contract,
					bMatches,
					ComparisonEvidence,
					OutOutcome);
				return true;
			}

		private:
			TSharedRef<IUeAgentWorldSpatialNativeEnvironment> Environment;
			TSharedRef<IUeAgentWorldSpatialSpaceCompiler> SpaceCompiler;
			TSharedRef<IUeAgentWorldSpatialRevisionSource> RevisionSource;
		};
	}

	FWorldSpatialNativeFamilyRef MakeViewportProjectionNativeFamily(
		const FUeAgentWorldSpatialNativeDependencies& Dependencies)
	{
		return MakeShared<FViewportProjectionNativeFamily>(Dependencies);
	}

	FWorldSpatialNativeFamilyRef MakeViewportSessionStateNativeFamily(
		const FUeAgentWorldSpatialNativeDependencies& Dependencies)
	{
		return MakeShared<FViewportSessionStateNativeFamily>(Dependencies);
	}
}
