#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialNativeInternal.h"
#include "Misc/ScopeExit.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialViewportWarnings.h"
#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialViewportStateContract.h"
#include "Core/ContentDiscovery/UeAgentContentDiscovery.h"
#include "Core/EditorViewport/UeAgentViewportViewMode.h"
#include "Core/EditorViewport/UeAgentViewportRealtimeState.h"
#include "Core/EditorCapture/UeAgentSlateWindowCapture.h"

#include "Components/PrimitiveComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EditorModeManager.h"
#include "EditorViewportClient.h"
#include "Engine/Selection.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "InteractiveToolManager.h"
#include "InteractiveToolsContext.h"
#include "SceneView.h"
#include "SEditorViewport.h"
#include "Slate/SceneViewport.h"
#include "Selection/GeometrySelectionManager.h"
#include "Selections/GeometrySelection.h"
#include "UnrealClient.h"
#include "UObject/Class.h"

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
		bool ViewportFailure(FUeAgentWorldSpatialExecutionError& OutError, const TCHAR* Code, const FString& Reason,
			const FString& Path, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, const FString& Target = FString())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("native_execution"));
			OutError.TargetPath = Target;
			OutError.PropertyPath = Path;
			OutError.Source.JsonPath = Path;
			OutError.SourceKind = TEXT("native_result");
			OutError.ActualValuePresent = Actual.IsValid();
			OutError.ActualValue = MoveTemp(Actual);
			OutError.ExpectedValue = MoveTemp(Expected);
			return false;
		}

		bool ViewportReadFailure(FUeAgentWorldSpatialExecutionError& OutError, const TCHAR* Code, const TCHAR* Reason,
			const FString& Role, const FString& Path, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
		{
			ViewportFailure(OutError, Code, FString::Printf(TEXT("Snapshot observation '%s' %s"), *Role, Reason),
				Path, MoveTemp(Actual), MoveTemp(Expected));
			OutError.SourceKind = TEXT("canonical_snapshot");
			OutError.SubjectId = Role;
			return false;
		}

		TSharedPtr<FJsonValue> ViewportSpatialFact(const TArray<double>& Components, const FString& Frame, const FString& Unit)
		{
			const TSharedRef<FJsonObject> Fact = MakeShared<FJsonObject>();
			Fact->SetStringField(TEXT("frame_id"), Frame);
			Fact->SetStringField(TEXT("unit"), Unit);
			Fact->SetField(TEXT("components"), ComparisonComponents(Components));
			return MakeShared<FJsonValueObject>(Fact);
		}

		TSharedPtr<FJsonValue> ViewportStrings(const TArray<FString>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> Items;
			for (const FString& Value : Values)
				Items.Add(MakeShared<FJsonValueString>(Value));
			return MakeShared<FJsonValueArray>(Items);
		}

		TSharedPtr<FJsonValue> ViewportObjectType(const UObject* Object)
		{
			return Object ? MakeShared<FJsonValueString>(Object->GetClass()->GetPathName())
				: TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>());
		}

		TSharedPtr<FJsonValue> ViewportJsonType(const TSharedPtr<FJsonValue>& Value)
		{
			if (!Value.IsValid()) return nullptr;
			const TCHAR* Type = TEXT("none");
			switch (Value->Type)
			{
			case EJson::Null: Type = TEXT("null"); break;
			case EJson::String: Type = TEXT("string"); break;
			case EJson::Number: Type = TEXT("number"); break;
			case EJson::Boolean: Type = TEXT("boolean"); break;
			case EJson::Array: Type = TEXT("array"); break;
			case EJson::Object: Type = TEXT("object"); break;
			default: break;
			}
			const TSharedRef<FJsonObject> Fact = MakeShared<FJsonObject>();
			Fact->SetStringField(TEXT("type"), Type);
			return MakeShared<FJsonValueObject>(Fact);
		}

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

		TSharedPtr<FJsonValue> ViewportTraceFact(const FViewportRay& Ray,
			const FWorldSpatialTraceQuery* Query, const FHitResult* Hit = nullptr)
		{
			const TSharedRef<FJsonObject> Fact = MakeShared<FJsonObject>();
			Fact->SetField(TEXT("origin"), ViewportSpatialFact({Ray.Origin.X, Ray.Origin.Y, Ray.Origin.Z}, TEXT("unreal.world"), TEXT("centimeter")));
			Fact->SetField(TEXT("direction"), ViewportSpatialFact({Ray.Direction.X, Ray.Direction.Y, Ray.Direction.Z}, TEXT("unreal.world"), TEXT("unitless")));
			if (Query)
			{
				Fact->SetStringField(TEXT("trace_channel"), Query->TraceChannel);
				Fact->SetField(TEXT("max_distance_cm"), ComparisonNumber(Query->MaxDistanceCm));
				Fact->SetBoolField(TEXT("trace_complex"), Query->bTraceComplex);
				Fact->SetNumberField(TEXT("ignored_target_count"), Query->IgnoreTargetRefs.Num());
			}
			if (Hit)
			{
				Fact->SetBoolField(TEXT("blocking_hit"), Hit->bBlockingHit);
				Fact->SetBoolField(TEXT("has_actor"), Hit->GetActor() != nullptr);
			}
			return MakeShared<FJsonValueObject>(Fact);
		}

		struct FViewportStateObservation
		{
			FUeAgentWorldSpatialCompiledValue CameraTransform;
			double FovDegrees = 0.0;
			FString ViewMode;
			bool bRealtime = false;
			bool bRealtimeConfigured = false;
			FString RealtimeOverrideReason;
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
			return FUeAgentCanonicalValue::TryString(
				Value, Context.GetResourceTracker(), OutValue, OutError);
		}

		bool TryMakeBooleanValue(
			bool Value,
			const FUeAgentOperationContext& Context,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			return FUeAgentCanonicalValue::TryBoolean(
				Value, Context.GetResourceTracker(), OutValue, OutError);
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
			return FUeAgentCanonicalValue::TryArray(
				Items, Context.GetResourceTracker(), OutValue, OutError);
		}

		bool TryMakeArray(
			const TArray<FUeAgentCanonicalValueRef>& Items,
			const FUeAgentOperationContext& Context,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			return FUeAgentCanonicalValue::TryArray(
				Items, Context.GetResourceTracker(), OutValue, OutError);
		}

		bool TryMakeObject(
			const TArray<TPair<FString, FUeAgentCanonicalValueRef>>& Items,
			const FUeAgentOperationContext& Context,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			return FUeAgentCanonicalValue::TryObject(
				Items, Context.GetResourceTracker(), OutValue, OutError);
		}

		bool TryMakeTemporalValue(
			const FUeAgentTemporalPoint& Value,
			const FUeAgentOperationContext& Context,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			return FUeAgentCanonicalValue::TryTemporal(
				Value, Context.GetResourceTracker(), OutValue, OutError);
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
			return FUeAgentCanonicalValue::TryArray(
				Items, Context.GetResourceTracker(), OutValue, OutError);
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
				return ViewportReadFailure(OutError, TEXT("world_spatial_viewport_boolean_readback_invalid"),
					!Entry ? TEXT("is missing.") : TEXT("must be a boolean."), Role, !Entry ? TEXT("/entries") : TEXT("/value"),
					Entry ? CanonicalFieldFact(Entry->GetValue()) : TSharedPtr<FJsonValue>(), MakeShared<FJsonValueString>(TEXT("boolean observation")));
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
				return ViewportReadFailure(OutError, TEXT("world_spatial_viewport_string_readback_invalid"),
					!Entry ? TEXT("is missing.") : TEXT("must be a string."), Role, !Entry ? TEXT("/entries") : TEXT("/value"),
					Entry ? CanonicalFieldFact(Entry->GetValue()) : TSharedPtr<FJsonValue>(), MakeShared<FJsonValueString>(TEXT("string observation")));
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
				return ViewportReadFailure(OutError, TEXT("world_spatial_viewport_integer_array_readback_invalid"),
					!Entry ? TEXT("is missing.") : TEXT("must be an array."), Role, !Entry ? TEXT("/entries") : TEXT("/value"),
					Entry ? CanonicalFieldFact(Entry->GetValue()) : TSharedPtr<FJsonValue>(), MakeShared<FJsonValueString>(TEXT("integer array")));
			}
			const auto& Items = Entry->GetValue()->GetArrayItems();
			for (int32 Index = 0; Index < Items.Num(); ++Index)
			{
				const auto& Item = Items[Index];
				int64 Parsed = 0;
				if ((Item->GetKind() != EUeAgentValueKind::Integer
						&& Item->GetKind() != EUeAgentValueKind::Decimal)
					|| !TryParseInt64Number(Item->GetLexeme(), Parsed))
				{
					return ViewportReadFailure(OutError, TEXT("world_spatial_viewport_integer_array_item_invalid"),
						TEXT("contains an element that cannot be read as a signed 64-bit integer."), Role,
						FString::Printf(TEXT("/value/%d"), Index), CanonicalFieldFact(Item), MakeShared<FJsonValueString>(TEXT("signed 64-bit integer")));
				}
				OutValues.Add(Parsed);
			}
			return true;
		}

		bool TryResolveViewportIdentity(
			IUeAgentWorldSpatialNativeEnvironment& Environment,
			const FString& Identity,
			FViewportBinding& Out,
			FUeAgentWorldSpatialExecutionError& OutError,
			const bool bRequireRenderSurface = true)
		{
			Out = {};
			Out.Identity = Identity;
			if (Out.Identity.IsEmpty())
				return ViewportFailure(OutError, TEXT("world_spatial_viewport_identity_unresolved"),
					TEXT("Viewport operations require an explicit viewport reference."), TEXT("/viewport_identity"),
					MakeShared<FJsonValueString>(Identity), MakeShared<FJsonValueString>(TEXT("non-empty viewport reference")), Identity);
			if (!Environment.TryResolveViewport(Out.Identity, Out.Client, Out.Viewport, OutError))
			{
				if (OutError.IsValid()) return false;
				return ViewportFailure(OutError, TEXT("world_spatial_viewport_identity_unresolved"),
					TEXT("The native viewport provider failed without returning a diagnostic."), TEXT("/viewport_identity"),
					MakeShared<FJsonValueString>(Identity), MakeShared<FJsonValueString>(TEXT("resolved viewport")), Identity);
			}
			if (!Out.Client || !Out.Viewport)
			{
				if (OutError.IsValid()) return false;
				return ViewportFailure(OutError, TEXT("world_spatial_viewport_identity_unresolved"),
					!Out.Client ? TEXT("The native viewport provider returned no viewport client.") : TEXT("The native viewport provider returned no viewport."),
					!Out.Client ? TEXT("/client") : TEXT("/viewport"), nullptr, MakeShared<FJsonValueString>(TEXT("bound viewport resource")), Identity);
			}
			if (Out.Client->Viewport != Out.Viewport)
			{
				if (OutError.IsValid()) return false;
				return ViewportFailure(OutError, TEXT("world_spatial_viewport_identity_unresolved"),
					TEXT("The viewport client is attached to a different viewport."), TEXT("/client/viewport"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Identity);
			}
			Out.World = Out.Client->GetWorld();
			if (!Out.World)
				return ViewportFailure(OutError, bRequireRenderSurface
					? TEXT("world_spatial_viewport_state_unavailable")
					: TEXT("world_spatial_viewport_world_unavailable"), TEXT("The viewport client has no world."),
					TEXT("/world"), nullptr, MakeShared<FJsonValueString>(TEXT("available viewport world")), Identity);
			const FIntPoint Size = Out.Viewport->GetSizeXY();
			if (!bRequireRenderSurface) return true;
			if (!Out.World->Scene)
				return ViewportFailure(OutError, TEXT("world_spatial_viewport_state_unavailable"),
					TEXT("The viewport world has no render scene."), TEXT("/scene"),
					nullptr, MakeShared<FJsonValueString>(TEXT("available render scene")), Identity);
			if (Size.X <= 0 || Size.Y <= 0)
				return ViewportFailure(OutError, TEXT("world_spatial_viewport_state_unavailable"),
					TEXT("The viewport render surface has a non-positive pixel size."), TEXT("/viewport/size"),
					ViewportSpatialFact({static_cast<double>(Size.X), static_cast<double>(Size.Y)}, TEXT("viewport.screen.pixel"), TEXT("pixel")),
					MakeShared<FJsonValueString>(TEXT("positive pixel width and height")), Identity);
			return true;
		}

		bool TryResolveViewportIdentityForCatalog(
			IUeAgentWorldSpatialNativeEnvironment& Environment,
			const FString& Identity,
			FViewportBinding& Out,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			return TryResolveViewportIdentity(Environment, Identity, Out, OutError, false);
		}

		bool TryResolveViewport(
			IUeAgentWorldSpatialNativeEnvironment& Environment,
			const FUeAgentOperationContext& Context,
			FViewportBinding& Out,
			FUeAgentWorldSpatialExecutionError& OutError,
			const bool bRequireRenderSurface = true)
		{
			return TryResolveViewportIdentity(
				Environment,
				Context.GetExecutionContext().GetViewportIdentity(),
				Out,
				OutError,
				bRequireRenderSurface);
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
				: ViewportFailure(OutError, TEXT("world_spatial_viewport_scene_view_failed"),
					TEXT("The viewport client could not generate a scene view."), TEXT("/scene_view"),
					nullptr, MakeShared<FJsonValueString>(TEXT("generated scene view")), Binding.Identity);
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
				return ViewportFailure(OutError, TEXT("world_spatial_viewport_subject_mismatch"),
					TEXT("The screen point subject does not match the bound viewport."), TEXT("/screen_point/subject"),
					MakeShared<FJsonValueString>(ScreenPoint.Subject), MakeShared<FJsonValueString>(Binding.Identity), Binding.Identity);
			}
			FUeAgentWorldSpatialCompiledValue Point;
			if (!Compiler.TryConvertToNative(
					ScreenPoint,
					TEXT("viewport.screen.pixel"),
					TEXT("pixel"),
					Point,
					OutError))
			{
				return false;
			}
			if (Point.Dimension != 2)
				return ViewportFailure(OutError, TEXT("world_spatial_viewport_screen_point_invalid"),
					TEXT("Screen deprojection requires a two-dimensional point."), TEXT("/screen_point/dimension"),
					MakeShared<FJsonValueNumber>(Point.Dimension), MakeShared<FJsonValueNumber>(2), Binding.Identity);
			if (Point.Quantity != TEXT("point"))
				return ViewportFailure(OutError, TEXT("world_spatial_viewport_screen_point_invalid"),
					TEXT("Screen deprojection requires a point quantity."), TEXT("/screen_point/quantity"),
					MakeShared<FJsonValueString>(Point.Quantity), MakeShared<FJsonValueString>(TEXT("point")), Binding.Identity);
			const FIntPoint Size = Binding.Viewport->GetSizeXY();
			if (Point.Components[0] < 0.0 || Point.Components[0] >= Size.X
				|| Point.Components[1] < 0.0 || Point.Components[1] >= Size.Y)
			{
				const TSharedRef<FJsonObject> Range = MakeShared<FJsonObject>();
				Range->SetStringField(TEXT("frame_id"), TEXT("viewport.screen.pixel"));
				Range->SetStringField(TEXT("unit"), TEXT("pixel"));
				Range->SetField(TEXT("minimum_inclusive"), ComparisonComponents({0.0, 0.0}));
				Range->SetField(TEXT("maximum_exclusive"), ComparisonComponents({static_cast<double>(Size.X), static_cast<double>(Size.Y)}));
				return ViewportFailure(OutError, TEXT("world_spatial_viewport_screen_point_out_of_bounds"),
					TEXT("The screen point lies outside the viewport's pixel bounds."), TEXT("/screen_point/components"),
					ViewportSpatialFact(Point.Components, Point.FrameId, Point.Unit), MakeShared<FJsonValueObject>(Range), Binding.Identity);
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
				: ViewportFailure(OutError, TEXT("world_spatial_viewport_deprojection_invalid"),
					TEXT("Screen deprojection produced a world ray with a zero direction within the engine's near-zero tolerance."), TEXT("/ray/direction"),
					ViewportSpatialFact({OutRay.Direction.X, OutRay.Direction.Y, OutRay.Direction.Z}, TEXT("unreal.world"), TEXT("unitless")),
					MakeShared<FJsonValueString>(TEXT("non-zero world ray direction")), Binding.Identity);
		}

		bool TryTraceChannel(
			const FString& Name,
			ECollisionChannel& OutChannel,
			FUeAgentWorldSpatialExecutionError& OutError,
			const FString& ViewportIdentity)
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
			TArray<TSharedPtr<FJsonValue>> Choices = {MakeShared<FJsonValueString>(TEXT("visibility")), MakeShared<FJsonValueString>(TEXT("camera"))};
			return ViewportFailure(OutError, TEXT("world_spatial_viewport_trace_channel_unsupported"),
				TEXT("The viewport trace channel is not supported."), TEXT("/trace_channel"),
				MakeShared<FJsonValueString>(Name), MakeShared<FJsonValueArray>(MoveTemp(Choices)), ViewportIdentity);
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
			if (!TryTraceChannel(Query.TraceChannel, Channel, OutError, Binding.Identity))
			{
				return false;
			}
			FCollisionQueryParams Params(
				TEXT("UAIWorldSpatialViewport"), Query.bTraceComplex);
			for (int32 Index = 0; Index < Query.IgnoreTargetRefs.Num(); ++Index)
			{
				const FString& Ref = Query.IgnoreTargetRefs[Index];
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
					TArray<TSharedPtr<FJsonValue>> Choices = {MakeShared<FJsonValueString>(TEXT("AActor")), MakeShared<FJsonValueString>(TEXT("UPrimitiveComponent"))};
					return ViewportFailure(OutError, TEXT("world_spatial_viewport_trace_ignore_type_invalid"),
						TEXT("A trace ignore reference must resolve to an Actor or primitive component."), FString::Printf(TEXT("/ignore_target_refs/%d"), Index),
						Object ? MakeShared<FJsonValueString>(Object->GetClass()->GetPathName()) : TSharedPtr<FJsonValue>(),
						MakeShared<FJsonValueArray>(MoveTemp(Choices)), Ref);
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

		FString ViewportViewModeId(FEditorViewportClient& Client)
		{
			return UeAgentViewportViewMode::DescribeViewportViewModeState(
				UeAgentViewportViewMode::CaptureViewportViewModeState(&Client)).Key;
		}

		bool TryResolveViewModeId(
			const FString& Id,
			UeAgentViewportViewMode::FResolvedViewportViewMode& Resolved,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("mode_key"), Id);
			bool bHasViewMode = false;
			if (!UeAgentViewportViewMode::TryResolveRequestedViewportViewMode(
					Params, bHasViewMode, Resolved, OutError))
			{
				return false;
			}
			if (!bHasViewMode)
			{
				return ViewportFailure(OutError, TEXT("world_spatial_viewport_view_mode_missing"),
					TEXT("The requested view mode must not be empty or whitespace."), TEXT("/view_mode"),
					MakeShared<FJsonValueString>(Id), MakeShared<FJsonValueString>(TEXT("nonempty view mode")));
			}
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

		bool TryResolveWidgetMode(
			const FString& Id,
			FEditorViewportClient& Client,
			UE::Widget::EWidgetMode& Mode,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			const FString Normalized = Id.TrimStartAndEnd().ToLower();
			Mode = UE::Widget::WM_None;
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
				return ViewportFailure(OutError, TEXT("world_spatial_viewport_widget_mode_unsupported"),
					TEXT("Widget mode is not supported."), TEXT("/interaction/widget_mode"),
					MakeShared<FJsonValueString>(Id), ViewportStrings({TEXT("none"), TEXT("translate"), TEXT("rotate"), TEXT("scale")}));
			}
			FEditorModeTools* ModeTools = Client.GetModeTools();
			if (!ModeTools)
			{
				return ViewportFailure(OutError, TEXT("world_spatial_viewport_mode_tools_missing"),
					TEXT("The viewport has no mode tools to apply the requested widget mode."), TEXT("/interaction/widget_mode"),
					MakeShared<FJsonValueString>(Id), MakeShared<FJsonValueString>(TEXT("available editor mode tools")));
			}
			if (Client.GetWidgetMode() == Mode)
			{
				return true;
			}
			if (ModeTools->IsTracking())
			{
				return ViewportFailure(OutError, TEXT("world_spatial_viewport_widget_mode_tracking"),
					TEXT("Widget mode cannot change while editor mode tools are tracking an interaction."), TEXT("/interaction/widget_mode"),
					MakeShared<FJsonValueString>(Id), MakeShared<FJsonValueString>(TEXT("no active tracking interaction")));
			}
			return true;
		}

		void ApplyResolvedWidgetMode(FEditorViewportClient& Client, UE::Widget::EWidgetMode Mode)
		{
			if (Client.GetWidgetMode() == Mode) return;
			if (Client.IsLevelEditorClient())
			{
				// The base setter ignores hidden viewport widgets, and CanSetWidgetMode
				// only reports the active editing modes' UI capability. Level viewport
				// state belongs to ModeTools and remains writable without that UI.
				Client.GetModeTools()->SetWidgetMode(Mode);
				Client.GetModeTools()->BroadcastWidgetModeChanged(Mode);
			}
			else
			{
				// Blueprint Components owns its mode in the viewport client itself.
				// Preserve the virtual setter instead of writing an unrelated manager.
				// Native visibility/override rules remain visible to readback.
				Client.SetWidgetMode(Mode);
			}
		}

		bool TryReadShowFlags(
			const FEditorViewportClient& Client,
			TArray<FString>& OutFlags,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			OutFlags.Reset();
			for (const FString& Name : GetViewportStateShowFlags())
			{
				const int32 Index = FEngineShowFlags::FindIndexByName(*Name);
				if (Index < 0)
				{
					return ViewportFailure(OutError, TEXT("world_spatial_viewport_show_flag_missing"),
						TEXT("The engine does not expose the requested show flag."), TEXT("/show_flags"),
						MakeShared<FJsonValueString>(Name), MakeShared<FJsonValueString>(TEXT("registered engine show flag")));
				}
				if (Client.EngineShowFlags.GetSingleFlag(Index))
				{
					OutFlags.Add(Name);
				}
			}
			return true;
		}

		bool TryResolveShowFlags(
			const TArray<FString>& Enabled,
			TArray<TPair<int32, bool>>& OutFlags,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			for (int32 EnabledIndex = 0; EnabledIndex < Enabled.Num(); ++EnabledIndex)
			{
				const FString& Name = Enabled[EnabledIndex];
				if (!GetViewportStateShowFlags().Contains(Name))
				{
					return ViewportFailure(OutError, TEXT("world_spatial_viewport_show_flag_unsupported"),
						TEXT("The show flag is not supported by viewport state updates."), FString::Printf(TEXT("/show_flags/%d"), EnabledIndex),
						MakeShared<FJsonValueString>(Name), ViewportStrings(GetViewportStateShowFlags()));
				}
			}
			for (const FString& Name : GetViewportStateShowFlags())
			{
				const int32 Index = FEngineShowFlags::FindIndexByName(*Name);
				if (Index < 0)
				{
					return ViewportFailure(OutError, TEXT("world_spatial_viewport_show_flag_missing"),
						TEXT("The engine does not expose the requested show flag."), TEXT("/show_flags"),
						MakeShared<FJsonValueString>(Name), MakeShared<FJsonValueString>(TEXT("registered engine show flag")));
				}
				OutFlags.Emplace(Index, Enabled.Contains(Name));
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
			const FString& Query,
			const FUeAgentOperationContext& Context,
			FUeAgentCanonicalValuePtr& OutValue,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			const TSharedPtr<FJsonObject> Catalog =
				UeAgentViewportViewMode::MakeViewportViewModeCatalogJson();
			const TArray<TSharedPtr<FJsonValue>>* Modes = nullptr;
			if (!Catalog.IsValid()
				|| !Catalog->TryGetArrayField(TEXT("modes"), Modes)
				|| !Modes || Modes->IsEmpty())
			{
				return ViewportFailure(OutError, TEXT("world_spatial_viewport_view_mode_catalog_missing"),
					Modes ? TEXT("The view mode catalog contains no modes.") : TEXT("The view mode provider did not return a modes array."), TEXT("/modes"),
					Modes ? MakeShared<FJsonValueNumber>(Modes->Num()) : (Catalog.IsValid() ? ViewportJsonType(Catalog->TryGetField(TEXT("modes"))) : TSharedPtr<FJsonValue>()),
					MakeShared<FJsonValueString>(TEXT("nonempty modes array")));
			}
			struct FModeMatch final
			{
				FString Id;
				FString Label;
				int32 Score = 0;
				int32 Order = 0;
			};
			TArray<FModeMatch> Matches;
			UE::UAI::ContentDiscovery::FContentDiscoveryRequest Search;
			Search.Query = Query;
			Search.MatchMode =
				UE::UAI::ContentDiscovery::EContentDiscoveryMatchMode::Text;
			int32 Order = 0;
			for (const TSharedPtr<FJsonValue>& ModeValue : *Modes)
			{
				const TSharedPtr<FJsonObject>* Mode = nullptr;
				FString ModeId;
				FString ModeLabel;
				const FString ModePath = FString::Printf(TEXT("/modes/%d"), Order);
				if (!ModeValue.IsValid() || !ModeValue->TryGetObject(Mode) || !Mode || !Mode->IsValid())
				{
					return ViewportFailure(OutError, TEXT("world_spatial_viewport_view_mode_catalog_entry_invalid"),
						TEXT("The view mode catalog entry must be an object."), ModePath, ViewportJsonType(ModeValue), MakeShared<FJsonValueString>(TEXT("object")));
				}
				if (!(*Mode)->TryGetStringField(TEXT("key"), ModeId))
					return ViewportFailure(OutError, TEXT("world_spatial_viewport_view_mode_catalog_entry_invalid"),
						TEXT("The view mode catalog entry requires a string key."), ModePath + TEXT("/key"),
						(*Mode)->TryGetField(TEXT("key")), MakeShared<FJsonValueString>(TEXT("string")));
				if (!(*Mode)->TryGetStringField(TEXT("label"), ModeLabel))
					return ViewportFailure(OutError, TEXT("world_spatial_viewport_view_mode_catalog_entry_invalid"),
						TEXT("The view mode catalog entry requires a string label."), ModePath + TEXT("/label"),
						(*Mode)->TryGetField(TEXT("label")), MakeShared<FJsonValueString>(TEXT("string")));
				if (ModeId.IsEmpty() || ModeLabel.IsEmpty())
				{
					return ViewportFailure(OutError, TEXT("world_spatial_viewport_view_mode_catalog_entry_invalid"),
						TEXT("View mode catalog keys and labels must not be empty."), ModePath + (ModeId.IsEmpty() ? TEXT("/key") : TEXT("/label")),
						MakeShared<FJsonValueString>(TEXT("")), MakeShared<FJsonValueString>(TEXT("nonempty string")));
				}
				int32 Score = 0;
				FString MatchReason;
				if (!Query.IsEmpty()
					&& !UE::UAI::ContentDiscovery::TryScoreContentDiscoveryMatch(
						Search,
						ModeId,
						ModeLabel,
						{},
						Score,
						MatchReason))
				{
					++Order;
					continue;
				}
				Matches.Add({ModeId, ModeLabel, Score, Order++});
			}
			Matches.Sort([](const FModeMatch& Left, const FModeMatch& Right)
			{
				return Left.Score != Right.Score
					? Left.Score > Right.Score
					: Left.Order < Right.Order;
			});
			TArray<FUeAgentCanonicalValueRef> Items;
			Items.Reserve(Matches.Num());
			for (const FModeMatch& Match : Matches)
			{
				FUeAgentCanonicalValuePtr Id;
				FUeAgentCanonicalValuePtr Label;
				FUeAgentCanonicalValuePtr Item;
				if (!TryMakeStringValue(Match.Id, Context, Id, OutError)
					|| !TryMakeStringValue(Match.Label, Context, Label, OutError)
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
			if (!BuildViewportWarnings(
					World, bIncludeSuppressed, WarningData, OutError))
			{
				return false;
			}
			const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
			if (!WarningData->TryGetArrayField(TEXT("warnings"), Warnings)
				|| !Warnings)
			{
				return ViewportFailure(OutError, TEXT("world_spatial_viewport_warning_probe_invalid"),
					TEXT("The warning provider did not return a warnings array."), TEXT("/warnings"),
					ViewportJsonType(WarningData->TryGetField(TEXT("warnings"))), MakeShared<FJsonValueString>(TEXT("array")));
			}
			TArray<FUeAgentCanonicalValueRef> Items;
			for (int32 WarningIndex = 0; WarningIndex < Warnings->Num(); ++WarningIndex)
			{
				const TSharedPtr<FJsonValue>& WarningValue = (*Warnings)[WarningIndex];
				const FString WarningPath = FString::Printf(TEXT("/warnings/%d"), WarningIndex);
				const TSharedPtr<FJsonObject>* Warning = nullptr;
				FString Code;
				FString Message;
				FString Severity;
				if (!WarningValue.IsValid() || !WarningValue->TryGetObject(Warning) || !Warning || !Warning->IsValid())
				{
					return ViewportFailure(OutError, TEXT("world_spatial_viewport_warning_entry_invalid"),
						TEXT("The warning entry must be an object."), WarningPath, ViewportJsonType(WarningValue), MakeShared<FJsonValueString>(TEXT("object")));
				}
				if (!(*Warning)->TryGetStringField(TEXT("code"), Code))
					return ViewportFailure(OutError, TEXT("world_spatial_viewport_warning_entry_invalid"),
						TEXT("The warning entry requires a string code."), WarningPath + TEXT("/code"),
						(*Warning)->TryGetField(TEXT("code")), MakeShared<FJsonValueString>(TEXT("string")));
				if (!(*Warning)->TryGetStringField(TEXT("text"), Message))
					return ViewportFailure(OutError, TEXT("world_spatial_viewport_warning_entry_invalid"),
						TEXT("The warning entry requires a string text."), WarningPath + TEXT("/text"),
						(*Warning)->TryGetField(TEXT("text")), MakeShared<FJsonValueString>(TEXT("string")));
				if (!(*Warning)->TryGetStringField(TEXT("severity"), Severity))
					return ViewportFailure(OutError, TEXT("world_spatial_viewport_warning_entry_invalid"),
						TEXT("The warning entry requires a string severity."), WarningPath + TEXT("/severity"),
						(*Warning)->TryGetField(TEXT("severity")), MakeShared<FJsonValueString>(TEXT("string")));
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
			const auto Realtime = EditorViewport::ReadRealtimeState(*Binding.Client);
			Out.bRealtime = Realtime.bEffective;
			Out.bRealtimeConfigured = Realtime.bConfigured;
			Out.RealtimeOverrideReason = Realtime.OverrideReason;
			Out.bGameView = Binding.Client->IsInGameView();
			Out.Interaction.bModeWidgets = Binding.Client->EngineShowFlags.ModeWidgets != 0;
			Out.Interaction.bSelection = Binding.Client->EngineShowFlags.Selection != 0;
			Out.Interaction.bPivot = Binding.Client->EngineShowFlags.Pivot != 0;
			Out.Interaction.bEditorPrimitives = Binding.Client->EngineShowFlags.Editor != 0;
			Out.Interaction.bGamePrimitives = Binding.Client->EngineShowFlags.Game != 0;
			Out.Interaction.WidgetMode = WidgetModeToId(
				Binding.Client->GetWidgetMode());
			if (!Out.Interaction.TryValidate(OutError))
			{
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Binding.Identity;
				return false;
			}
			Out.Interaction.WidgetMode.ToLowerInline();
			const bool bValid = Out.CameraTransform.TryValidate(OutError)
				&& TryReadShowFlags(*Binding.Client, Out.ShowFlags, OutError)
				&& RevisionSource.TryReadRevision(
					Binding.Identity, Out.Revision, OutError)
				&& Out.Revision.TryValidate(OutError);
			if (!bValid && OutError.TargetPath.IsEmpty()) OutError.TargetPath = Binding.Identity;
			return bValid;
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
			FUeAgentCanonicalValuePtr RealtimeConfigured;
			FUeAgentCanonicalValuePtr RealtimeOverrideReason;
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
				|| !TryMakeBooleanValue(State.bRealtimeConfigured, Context, RealtimeConfigured, OutError)
				|| !TryMakeStringValue(State.RealtimeOverrideReason, Context, RealtimeOverrideReason, OutError)
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
					TEXT("%s|%s|%.9g|%d|%d|%s|%d|%s|%d,%d,%d,%d,%d,%s|%s|%s|%lld"),
					*Owner,
					*State.CameraTransform.EvidenceId,
					State.FovDegrees,
					State.bRealtime,
					State.bRealtimeConfigured,
					*State.RealtimeOverrideReason,
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
					{TEXT("world-spatial:viewport:realtime-configured"), TEXT("viewport"), Owner, Owner, TEXT("viewport_state.realtime_configured"), TEXT("core.boolean"), RealtimeConfigured},
					{TEXT("world-spatial:viewport:realtime-override"), TEXT("viewport"), Owner, Owner, TEXT("viewport_state.realtime_override_reason"), TEXT("core.string"), RealtimeOverrideReason},
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

		bool TryResolveCameraTransform(
			IUeAgentWorldSpatialSpaceCompiler& Compiler,
			const FWorldSpatialValue& Transform,
			const FString& Mode,
			FEditorViewportClient& Client,
			FVector& OutLocation,
			FRotator& OutRotation,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			const bool bAbsolute = Mode == TEXT("absolute");
			const bool bDelta = Mode == TEXT("delta");
			const bool bWorldFrame = Transform.Frame.FrameId == TEXT("unreal.world");
			const bool bLocalFrame =
				Transform.Frame.FrameId == TEXT("viewport.camera.local");
			if (!bAbsolute && !bDelta)
			{
				return ViewportFailure(OutError, TEXT("world_spatial_viewport_camera_transform_mode_invalid"),
					TEXT("Camera transform mode must be absolute or delta."), TEXT("/camera_transform_mode"),
					MakeShared<FJsonValueString>(Mode), ViewportStrings({TEXT("absolute"), TEXT("delta")}), Transform.Subject);
			}
			if ((bAbsolute && !bWorldFrame) || (bDelta && !bWorldFrame && !bLocalFrame))
			{
				return ViewportFailure(OutError, TEXT("world_spatial_viewport_camera_transform_mode_invalid"),
					bAbsolute ? TEXT("Absolute camera transforms require the world frame.")
						: TEXT("Delta camera transforms require the world or camera-local frame."), TEXT("/camera_transform/frame/frame_id"),
					MakeShared<FJsonValueString>(Transform.Frame.FrameId),
					bAbsolute ? ViewportStrings({TEXT("unreal.world")})
						: ViewportStrings({TEXT("unreal.world"), TEXT("viewport.camera.local")}), Transform.Subject);
			}
			FUeAgentWorldSpatialCompiledValue Compiled;
			if (!Compiler.TryConvertToNative(
					Transform,
					bLocalFrame
						? TEXT("viewport.camera.local") : TEXT("unreal.world"),
					TEXT("centimeter"),
					Compiled,
					OutError))
			{
				return false;
			}
			if (Compiled.Quantity != TEXT("transform"))
			{
				return ViewportFailure(OutError, TEXT("world_spatial_viewport_camera_transform_invalid"),
					TEXT("The compiled camera value must be a transform."), TEXT("/camera_transform/quantity"),
					MakeShared<FJsonValueString>(Compiled.Quantity), MakeShared<FJsonValueString>(TEXT("transform")), Transform.Subject);
			}
			if (Compiled.Dimension != 3)
			{
				return ViewportFailure(OutError, TEXT("world_spatial_viewport_camera_transform_invalid"),
					TEXT("The compiled camera transform must have three dimensions."), TEXT("/camera_transform/dimension"),
					MakeShared<FJsonValueNumber>(Compiled.Dimension), MakeShared<FJsonValueNumber>(3), Transform.Subject);
			}
			if (!FMath::IsNearlyEqual(Compiled.Components[7], 1.0)
				|| !FMath::IsNearlyEqual(Compiled.Components[8], 1.0)
				|| !FMath::IsNearlyEqual(Compiled.Components[9], 1.0))
			{
				return ViewportFailure(OutError, TEXT("world_spatial_viewport_camera_scale_unsupported"),
					TEXT("Camera transforms require unit scale on all three axes."), TEXT("/camera_transform/scale"),
					ViewportSpatialFact({Compiled.Components[7], Compiled.Components[8], Compiled.Components[9]}, Compiled.FrameId, TEXT("unitless")),
					ViewportSpatialFact({1.0, 1.0, 1.0}, Compiled.FrameId, TEXT("unitless")), Transform.Subject);
			}
			const FQuat Rotation(
				Compiled.Components[3],
				Compiled.Components[4],
				Compiled.Components[5],
				Compiled.Components[6]);
			if (!Rotation.IsNormalized())
			{
				ViewportFailure(OutError, TEXT("world_spatial_viewport_camera_rotation_invalid"),
					TEXT("Camera rotation must be a normalized quaternion (x, y, z, w)."), TEXT("/camera_transform/rotation"),
					ViewportSpatialFact({Rotation.X, Rotation.Y, Rotation.Z, Rotation.W}, Compiled.FrameId, TEXT("unitless")),
					MakeShared<FJsonValueString>(TEXT("normalized quaternion_xyzw")), Transform.Subject);
				OutError.ActualValue->AsObject()->SetStringField(TEXT("representation"), TEXT("quaternion_xyzw"));
				return false;
			}
			const FVector Translation(
				Compiled.Components[0],
				Compiled.Components[1],
				Compiled.Components[2]);
			const FVector CurrentLocation = Client.GetViewLocation();
			const FQuat CurrentRotation = Client.GetViewRotation().Quaternion();
			const FVector TargetLocation = bAbsolute
				? Translation
				: CurrentLocation + (bLocalFrame
					? CurrentRotation.RotateVector(Translation) : Translation);
			const FQuat TargetRotation = bAbsolute
				? Rotation
				: bLocalFrame
					? CurrentRotation * Rotation : Rotation * CurrentRotation;
			OutLocation = TargetLocation;
			OutRotation = TargetRotation.Rotator();
			return true;
		}

		bool TryApplyViewportState(
			IUeAgentWorldSpatialSpaceCompiler& Compiler,
			const FWorldSpatialViewportStatePatch& Patch,
			FViewportBinding& Binding,
			FUeAgentWorldSpatialMutationResult& OutResult,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			// Resolve every fallible input before the first setter. Both update and
			// restore use this path, so invalid later fields cannot leave partial state.
			OutResult = FUeAgentWorldSpatialMutationResult::NoMutation();
			FVector CameraLocation;
			FRotator CameraRotation;
			UeAgentViewportViewMode::FResolvedViewportViewMode ViewMode;
			UE::Widget::EWidgetMode WidgetMode = UE::Widget::WM_None;
			TArray<TPair<int32, bool>> ShowFlags;
			TArray<TPair<int32, bool>> InteractionFlags;
			if (Patch.CameraTransform.IsSet()
				&& !TryResolveCameraTransform(
					Compiler,
					Patch.CameraTransform.GetValue(),
					Patch.CameraTransformMode.GetValue(),
					*Binding.Client,
					CameraLocation, CameraRotation, OutError))
			{
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Binding.Identity;
				return false;
			}
			if (Patch.ViewMode.IsSet()
				&& !TryResolveViewModeId(Patch.ViewMode.GetValue(), ViewMode, OutError))
			{
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Binding.Identity;
				OutError.PropertyPath = TEXT("/view_mode");
				OutError.Source.JsonPath = OutError.PropertyPath;
				return false;
			}
			if (Patch.ShowFlags.IsSet()
				&& !TryResolveShowFlags(Patch.ShowFlags.GetValue(), ShowFlags, OutError))
			{
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Binding.Identity;
				return false;
			}
			if (Patch.Interaction.IsSet())
			{
				const FWorldSpatialViewportInteractionState& Interaction =
					Patch.Interaction.GetValue();
				if (!TryResolveWidgetMode(
						Interaction.WidgetMode,
						*Binding.Client,
						WidgetMode, OutError))
				{
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Binding.Identity;
					return false;
				}
				for (const TPair<FString, bool>& Flag : {
					TPair<FString, bool>(TEXT("Editor"), Interaction.bEditorPrimitives),
					TPair<FString, bool>(TEXT("Game"), Interaction.bGamePrimitives)})
				{
					const int32 Index = FEngineShowFlags::FindIndexByName(*Flag.Key);
					if (Index < 0)
					{
						return ViewportFailure(OutError, TEXT("world_spatial_viewport_interaction_flag_missing"),
							TEXT("The engine does not expose the interaction show flag."), TEXT("/interaction"),
							MakeShared<FJsonValueString>(Flag.Key), MakeShared<FJsonValueString>(TEXT("registered engine show flag")), Binding.Identity);
					}
					InteractionFlags.Emplace(Index, Flag.Value);
				}
			}
			// All remaining operations are native setters with resolved values. Keep
			// their existing order because view mode/game view also update show flags.
			OutResult = {};
			if (Patch.CameraTransform.IsSet())
			{
				Binding.Client->SetViewLocation(CameraLocation);
				Binding.Client->SetViewRotation(CameraRotation);
			}
			if (Patch.FovDegrees.IsSet()) Binding.Client->ViewFOV = Patch.FovDegrees.GetValue();
			if (Patch.ViewMode.IsSet()) UeAgentViewportViewMode::ApplyResolvedViewportViewMode(Binding.Client, ViewMode);
			if (Patch.Realtime.IsSet()) EditorViewport::SetRealtimePreference(*Binding.Client, Patch.Realtime.GetValue());
			if (Patch.GameView.IsSet()) Binding.Client->SetGameView(Patch.GameView.GetValue());
			for (const auto& Flag : ShowFlags) Binding.Client->EngineShowFlags.SetSingleFlag(Flag.Key, Flag.Value);
			if (Patch.Interaction.IsSet())
			{
				const auto& Interaction = Patch.Interaction.GetValue();
				Binding.Client->EngineShowFlags.SetModeWidgets(Interaction.bModeWidgets);
				Binding.Client->EngineShowFlags.SetSelection(Interaction.bSelection);
				Binding.Client->EngineShowFlags.SetPivot(Interaction.bPivot);
				ApplyResolvedWidgetMode(*Binding.Client, WidgetMode);
				for (const auto& Flag : InteractionFlags) Binding.Client->EngineShowFlags.SetSingleFlag(Flag.Key, Flag.Value);
			}
			Binding.Client->Invalidate();
			return true;
		}

		FWorldSpatialViewportStatePatch MakePatch(
			const FWorldSpatialViewportState& State)
		{
			FWorldSpatialViewportStatePatch Patch;
			Patch.CameraTransform = State.Camera.CameraTransform;
			Patch.CameraTransformMode = TEXT("absolute");
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
			TArray<AActor*>& OutActors,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			OutBounds = FBox(ForceInit);
			for (int32 SubjectIndex = 0; SubjectIndex < SubjectRefs.Num(); ++SubjectIndex)
			{
				const FString& Ref = SubjectRefs[SubjectIndex];
				UObject* Object = nullptr;
				if (!Environment.TryResolveObject(Ref, Object, OutError))
				{
					return false;
				}
				if (AActor* Actor = Cast<AActor>(Object))
				{
					OutActors.AddUnique(Actor);
					OutBounds += Actor->GetComponentsBoundingBox(true);
				}
				else if (UPrimitiveComponent* Component =
					Cast<UPrimitiveComponent>(Object))
				{
					if (AActor* Owner = Component->GetOwner())
					{
						OutActors.AddUnique(Owner);
					}
					OutBounds += Component->Bounds.GetBox();
				}
				else
				{
					return ViewportFailure(OutError, TEXT("world_spatial_viewport_frame_subject_type_invalid"),
						TEXT("A camera-framing subject must resolve to an Actor or PrimitiveComponent."), FString::Printf(TEXT("/subjects/%d"), SubjectIndex),
						ViewportObjectType(Object), ViewportStrings({AActor::StaticClass()->GetPathName(), UPrimitiveComponent::StaticClass()->GetPathName()}), Ref);
				}
			}
			if (OutBounds.IsValid) return true;
			return ViewportFailure(OutError, TEXT("world_spatial_viewport_frame_bounds_invalid"),
				TEXT("The resolved subjects did not provide a valid combined bounding box."), TEXT("/bounds"),
				ViewportStrings(SubjectRefs), MakeShared<FJsonValueString>(TEXT("subjects with valid native bounds")));
		}

		bool TryResolveFrameBounds(
			IUeAgentWorldSpatialNativeEnvironment& Environment,
			IUeAgentWorldSpatialSpaceCompiler& Compiler,
			const FViewportCameraFrameRequestInput& Input,
			const FViewportBinding& Binding,
			FBox& OutBounds,
			TArray<AActor*>& OutActors,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			OutBounds = FBox(ForceInit);
			OutActors.Reset();
			if (Input.WorldPoint.IsSet() || Input.WorldBoundsMin.IsSet())
			{
				auto CompilePoint = [&](const FWorldSpatialValue& Value,
					const FString& Path, FVector& OutPoint)
				{
					FUeAgentWorldSpatialCompiledValue Compiled;
					if (!Compiler.TryConvertToNative(
							Value,
							TEXT("unreal.world"),
							TEXT("centimeter"),
							Compiled,
							OutError))
					{
						return false;
					}
					if (Compiled.Quantity != TEXT("point"))
						return ViewportFailure(OutError, TEXT("world_spatial_viewport_frame_point_invalid"),
							TEXT("The compiled camera-framing value must be a point."), Path + TEXT("/quantity"),
							MakeShared<FJsonValueString>(Compiled.Quantity), MakeShared<FJsonValueString>(TEXT("point")), Binding.Identity);
					if (Compiled.Dimension != 3)
						return ViewportFailure(OutError, TEXT("world_spatial_viewport_frame_point_invalid"),
							TEXT("The compiled camera-framing point must have three dimensions."), Path + TEXT("/dimension"),
							MakeShared<FJsonValueNumber>(Compiled.Dimension), MakeShared<FJsonValueNumber>(3), Binding.Identity);
					OutPoint = FVector(
						Compiled.Components[0],
						Compiled.Components[1],
						Compiled.Components[2]);
					return true;
				};
				if (Input.WorldPoint.IsSet())
				{
					FVector Point;
					if (!CompilePoint(Input.WorldPoint.GetValue(), TEXT("/world_point"), Point))
					{
						return false;
					}
					OutBounds = FBox(Point - FVector(0.5), Point + FVector(0.5));
				}
				else
				{
					FVector Min = FVector::ZeroVector;
					FVector Max = FVector::ZeroVector;
					if (!CompilePoint(Input.WorldBoundsMin.GetValue(), TEXT("/world_bounds/min"), Min)
						|| !CompilePoint(Input.WorldBoundsMax.GetValue(), TEXT("/world_bounds/max"), Max))
					{
						return false;
					}
					if (Min.X > Max.X || Min.Y > Max.Y || Min.Z > Max.Z)
					{
						ViewportFailure(OutError, TEXT("world_spatial_viewport_frame_world_bounds_invalid"),
							TEXT("World bounds require min to be no greater than max on each axis."), TEXT("/world_bounds"),
							ViewportSpatialFact({Min.X, Min.Y, Min.Z}, TEXT("unreal.world"), TEXT("centimeter")),
							ViewportSpatialFact({Max.X, Max.Y, Max.Z}, TEXT("unreal.world"), TEXT("centimeter")), Binding.Identity);
						OutError.ActualValue->AsObject()->SetStringField(TEXT("role"), TEXT("min"));
						OutError.ExpectedValue->AsObject()->SetStringField(TEXT("role"), TEXT("max_inclusive"));
						return false;
					}
					OutBounds = FBox(Min, Max);
				}
			}
			else if (!Input.SubjectRefs.IsEmpty())
			{
				if (!TryBoundsForSubjects(
						Environment, Input.SubjectRefs, OutBounds, OutActors, OutError))
				{
					return false;
				}
			}
			else if (Input.FolderPath.IsSet())
			{
				if (!GetFolderActorBounds(
						Binding.World,
						Input.FolderPath.GetValue(),
						Input.bIncludeChildFolders,
						OutBounds,
						OutActors,
						OutError))
				{
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Binding.Identity;
					return false;
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
			if (OutBounds.IsValid) return true;
			return ViewportFailure(OutError, TEXT("world_spatial_viewport_frame_selector_empty"),
				Input.bUseCurrentSelection ? TEXT("The current selection did not provide valid bounds in the viewport world.")
					: TEXT("The camera-framing selector did not provide valid bounds."), TEXT("/bounds"),
				MakeShared<FJsonValueNumber>(OutActors.Num()), MakeShared<FJsonValueString>(TEXT("valid bounds from the requested selector")), Binding.Identity);
		}

		bool TryFrameCamera(
			IUeAgentWorldSpatialNativeEnvironment& Environment,
			IUeAgentWorldSpatialSpaceCompiler& Compiler,
			const FViewportCameraFrameRequestInput& Input,
			FViewportBinding& Binding,
			FUeAgentWorldSpatialMutationResult& OutResult,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			FBox Bounds;
			TArray<AActor*> Actors;
			if (!TryResolveFrameBounds(
					Environment, Compiler, Input, Binding, Bounds, Actors, OutError))
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
			OutResult = {};
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
			for (int32 ActorIndex = 0; ActorIndex < ActorRefs.Num(); ++ActorIndex)
			{
				const FString& Ref = ActorRefs[ActorIndex];
				UObject* Object = nullptr;
				if (!Environment.TryResolveObject(Ref, Object, OutError))
				{
					return false;
				}
				AActor* Actor = Cast<AActor>(Object);
				if (!Actor)
				{
					return ViewportFailure(OutError, TEXT("world_spatial_viewport_selection_actor_invalid"),
						TEXT("An actor selection reference must resolve to an Actor."), FString::Printf(TEXT("/actor_refs/%d"), ActorIndex),
						ViewportObjectType(Object), MakeShared<FJsonValueString>(AActor::StaticClass()->GetPathName()), Ref);
				}
				OutActors.AddUnique(Actor);
			}
			if (!OutActors.IsEmpty()) return true;
			return ViewportFailure(OutError, TEXT("world_spatial_viewport_selection_actor_set_empty"),
				TEXT("Actor selection requires at least one resolved Actor."), TEXT("/actor_refs"),
				ViewportStrings(ActorRefs), MakeShared<FJsonValueString>(TEXT("nonempty Actor reference array")));
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
			return ViewportFailure(OutError, TEXT("world_spatial_mesh_selection_mode_unsupported"),
				TEXT("The mesh selection mode is not supported."), TEXT("/selection_mode"), MakeShared<FJsonValueString>(Mode),
				ViewportStrings({TEXT("replace"), TEXT("add"), TEXT("remove")}));
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
				return ViewportFailure(OutError, TEXT("world_spatial_mesh_element_kind_unsupported"),
					TEXT("The mesh element kind is not supported."), TEXT("/element_kind"), MakeShared<FJsonValueString>(Kind),
					ViewportStrings({TEXT("vertex"), TEXT("edge"), TEXT("triangle"), TEXT("polygon")}));
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
			const auto SessionFailure = [&OutError, &Context](const TCHAR* Code, const TCHAR* Reason, const TCHAR* Path,
				TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
			{
				return ViewportFailure(OutError, Code, Reason, Path, MoveTemp(Actual), MoveTemp(Expected),
					Context.GetExecutionContext().GetViewportIdentity());
			};
			if (!OutManager)
				return SessionFailure(TEXT("world_spatial_mesh_selection_session_invalid"), TEXT("The resolved mesh selection session has no manager."),
					TEXT("/manager"), nullptr, MakeShared<FJsonValueString>(TEXT("selection manager")));
			if (OutTargets.IsEmpty())
				return SessionFailure(TEXT("world_spatial_mesh_selection_session_invalid"), TEXT("The resolved mesh selection session contains no targets."),
					TEXT("/targets"), ComparisonNumber(0), MakeShared<FJsonValueString>(TEXT("at least one target")));
			for (int32 Index = 0; Index < OutTargets.Num(); ++Index)
			{
				if (!OutTargets[Index].TryValidate(OutError))
				{
					UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, FString::Printf(TEXT("/targets/%d"), Index));
					return false;
				}
			}
			UInteractiveToolsContext* ToolsContext =
				OutManager->GetToolsContext();
			if (!ToolsContext || !ToolsContext->ToolManager
				|| !ToolsContext->ToolManager->GetContextQueriesAPI()
				|| !OutManager->GetTransactionsAPI())
			{
				const TCHAR* Path = !ToolsContext ? TEXT("/tools_context")
					: (!ToolsContext->ToolManager ? TEXT("/tools_context/tool_manager")
						: (!ToolsContext->ToolManager->GetContextQueriesAPI() ? TEXT("/tools_context/context_queries_api") : TEXT("/transactions_api")));
				return SessionFailure(TEXT("world_spatial_mesh_selection_tools_context_invalid"),
					TEXT("The mesh selection session is missing a required tools service."), Path,
					nullptr, MakeShared<FJsonValueString>(TEXT("available tools service")));
			}
			if (OutManager->GetAnyCurrentTargetsLocked())
			{
				return SessionFailure(TEXT("world_spatial_mesh_selection_target_locked"), TEXT("The mesh selection manager reports locked current targets."),
					TEXT("/targets_locked"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
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

				TOptional<FIntPoint> ColdSize;
				if (Request.GetPayload().IsType<FViewportRayDeprojectRequestInput>())
					ColdSize = Request.GetPayload().Get<FViewportRayDeprojectRequestInput>().ColdStartRenderSize;
				else if (Request.GetPayload().IsType<FViewportRayTraceRequestInput>())
					ColdSize = Request.GetPayload().Get<FViewportRayTraceRequestInput>().ColdStartRenderSize;
				FViewportBinding Binding;
				if (!TryResolveViewport(*Environment, Context, Binding, OutError, !ColdSize.IsSet()))
				{
					return false;
				}
				UE::UAI::EditorCapture::SlateWindow::FScopedViewportExtent ScopedExtent;
				if (ColdSize.IsSet())
				{
					const auto Widget = Binding.Client->GetEditorViewportWidget();
					const auto SceneViewport = Widget ? Widget->GetSceneViewport() : nullptr;
					if (!SceneViewport || SceneViewport.Get() != Binding.Viewport)
						return ViewportFailure(OutError, TEXT("world_spatial_viewport_state_unavailable"),
							TEXT("Cold-start ray queries require the bound editor scene viewport."), TEXT("/viewport"),
							nullptr, MakeShared<FJsonValueString>(TEXT("matching editor scene viewport")), Binding.Identity);
					if (!ScopedExtent.Prepare(SceneViewport, &ColdSize.GetValue(), TEXT("explicit_query_size"), OutError)
						|| !TryResolveViewport(*Environment, Context, Binding, OutError)) return false;
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
					if (!View->WorldToPixel(WorldPoint, Pixel))
					{
						return ViewportFailure(OutError, TEXT("world_spatial_viewport_pixel_scale_projection_failed"),
							TEXT("The sample point could not be projected into viewport pixels."), TEXT("/sample_point"),
							ViewportSpatialFact({WorldPoint.X, WorldPoint.Y, WorldPoint.Z}, TEXT("unreal.world"), TEXT("centimeter")),
							MakeShared<FJsonValueString>(TEXT("projectable world point")), Sample.Subject);
					}
					const FVector Neighbor = WorldPoint + CameraRight;
					if (!View->WorldToPixel(Neighbor, NeighborPixel))
					{
						return ViewportFailure(OutError, TEXT("world_spatial_viewport_pixel_scale_projection_failed"),
							TEXT("The neighbor one centimeter to camera-right of the sample could not be projected into viewport pixels."), TEXT("/neighbor_sample"),
							ViewportSpatialFact({Neighbor.X, Neighbor.Y, Neighbor.Z}, TEXT("unreal.world"), TEXT("centimeter")),
							MakeShared<FJsonValueString>(TEXT("projectable world point")), Sample.Subject);
					}
					const double PixelDistance = FVector2D::Distance(Pixel, NeighborPixel);
					if (PixelDistance <= UE_SMALL_NUMBER)
					{
						return ViewportFailure(OutError, TEXT("world_spatial_viewport_pixel_scale_degenerate"),
							TEXT("The projected one-centimeter sample separation is too small to compute pixel scale."), TEXT("/pixel_distance"),
							ViewportSpatialFact({PixelDistance}, TEXT("viewport.screen.pixel"), TEXT("pixel")),
							ViewportSpatialFact({UE_SMALL_NUMBER}, TEXT("viewport.screen.pixel"), TEXT("pixel")), Sample.Subject);
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
						return ViewportFailure(OutError, TEXT("world_spatial_viewport_actor_selection_selector_missing"),
							TEXT("Actor selection requires actor references or a screen point."), TEXT("/screen_point"),
							nullptr, MakeShared<FJsonValueString>(TEXT("screen point when actor_refs is empty")), Binding.Identity);
					}
					ScreenPoint = &Input.ScreenPoint.GetValue();
					TraceQuery = Input.TraceQuery.IsSet()
						? Input.TraceQuery.GetValue() : DefaultTraceQuery();
					bSelectionReadback = true;
				}
				if (!ScreenPoint)
				{
					return ViewportFailure(OutError, TEXT("world_spatial_viewport_projection_request_mismatch"),
						TEXT("The request payload is not supported by the viewport projection reader."), TEXT("/request"),
						MakeShared<FJsonValueString>(Contract.GetCapabilityId()),
						MakeShared<FJsonValueString>(TEXT("pixel-scale, ray, actor-pick, or selection request")), Binding.Identity);
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
					return ViewportFailure(OutError, TEXT("world_spatial_viewport_actor_pick_hit_missing"),
						Hit->bBlockingHit ? TEXT("The blocking trace hit has no Actor to pick.") : TEXT("Actor picking requires a blocking hit, but the trace missed."), TEXT("/trace_result"),
						ViewportTraceFact(Ray, TraceQuery.IsSet() ? &TraceQuery.GetValue() : nullptr, &Hit.GetValue()),
						MakeShared<FJsonValueString>(TEXT("blocking hit with an Actor")), Binding.Identity);
				}
				if (!bSelectionReadback)
				{
					return TrySnapshotRay(
						Contract, Phase, Binding, Ray, Hit, *Environment,
						Context, OutSnapshot, OutError);
				}
				if (!Hit.IsSet() || !Hit->bBlockingHit || !Hit->GetActor())
				{
					return ViewportFailure(OutError, TEXT("world_spatial_viewport_actor_selection_hit_missing"),
						!Hit.IsSet() ? TEXT("Actor selection has no trace result.") : !Hit->bBlockingHit
							? TEXT("Actor selection requires a blocking hit, but the trace missed.") : TEXT("The blocking trace hit has no Actor to select."), TEXT("/trace_result"),
						ViewportTraceFact(Ray, TraceQuery.IsSet() ? &TraceQuery.GetValue() : nullptr, Hit.IsSet() ? &Hit.GetValue() : nullptr),
						MakeShared<FJsonValueString>(TEXT("blocking hit with an Actor")), Binding.Identity);
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
				const FUeAgentWorldSpatialPreflightFence& Fence,
				const FUeAgentOperationContext& Context,
				FUeAgentWorldSpatialMutationResult& OutResult,
				FUeAgentWorldSpatialExecutionError& OutError) override
			{
				OutResult = FUeAgentWorldSpatialMutationResult::NoMutation();
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
						if (!Input.ScreenPoint.IsSet())
						{
							return ViewportFailure(OutError, TEXT("world_spatial_viewport_actor_selection_hit_missing"),
								TEXT("Actor selection requires actor references or a screen point."), TEXT("/screen_point"),
								nullptr, MakeShared<FJsonValueString>(TEXT("screen point when actor_refs is empty")), Binding.Identity);
						}
						if (!TryDeproject(
								*SpaceCompiler, Input.ScreenPoint.GetValue(), Binding, Ray, OutError)
							|| !TryTrace(
								*Environment, Binding, Ray, Query, Hit, OutError))
						{
							return false;
						}
						if (!Hit.bBlockingHit || !Hit.GetActor())
						{
							return ViewportFailure(OutError, TEXT("world_spatial_viewport_actor_selection_hit_missing"),
								Hit.bBlockingHit ? TEXT("The blocking trace hit has no Actor to select.") : TEXT("Actor selection requires a blocking hit, but the trace missed."), TEXT("/trace_result"),
								ViewportTraceFact(Ray, &Query, &Hit), MakeShared<FJsonValueString>(TEXT("blocking hit with an Actor")), Binding.Identity);
						}
						Actors.Add(Hit.GetActor());
					}
					if (!GEditor)
					{
						return ViewportFailure(OutError, TEXT("world_spatial_viewport_actor_selection_editor_missing"),
							TEXT("The Editor is unavailable to apply actor selection."), TEXT("/editor"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueString>(TEXT("available Editor")), Binding.Identity);
					}
					if (Input.SelectionMode == TEXT("replace"))
					{
						OutResult = {};
						GEditor->SelectNone(false, true, false);
					}
					for (AActor* Actor : Actors)
					{
						const bool bWasSelected = GEditor->GetSelectedActors()
							&& GEditor->GetSelectedActors()->IsSelected(Actor);
						if (Input.SelectionMode == TEXT("replace")
							|| Input.SelectionMode == TEXT("add"))
						{
							OutResult = {};
							GEditor->SelectActor(Actor, true, true, true);
						}
						else if (Input.SelectionMode == TEXT("remove"))
						{
							OutResult = {};
							GEditor->SelectActor(Actor, false, true, true);
						}
						else if (Input.SelectionMode == TEXT("toggle"))
						{
							OutResult = {};
							GEditor->SelectActor(Actor, !bWasSelected, true, true);
						}
						else
						{
							return ViewportFailure(OutError, TEXT("world_spatial_viewport_actor_selection_mode_unsupported"),
								TEXT("The actor selection mode is not supported."), TEXT("/selection_mode"), MakeShared<FJsonValueString>(Input.SelectionMode),
								ViewportStrings({TEXT("replace"), TEXT("add"), TEXT("remove"), TEXT("toggle")}), Binding.Identity);
						}
					}
					OutResult = FUeAgentWorldSpatialMutationResult(
						TEXT("world-spatial:") + Context.GetRequestId(),
						TEXT("explicit-editor-selection-write:") + Binding.Identity);
					return OutResult.TryValidate(OutError);
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
							return ViewportFailure(OutError, TEXT("world_spatial_mesh_selection_ray_missed"),
								TEXT("The mesh selection trace filter found no blocking hit."), TEXT("/trace_result"),
								ViewportTraceFact(Ray, &Input.TraceQuery.GetValue(), &FilterHit), MakeShared<FJsonValueString>(TEXT("blocking hit on a selection target")), Binding.Identity);
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
							TArray<FString> TargetRefs;
							for (const auto& SelectionTarget : SelectionTargets) TargetRefs.Add(SelectionTarget.ExplicitRef);
							ViewportFailure(OutError, TEXT("world_spatial_mesh_selection_ray_occluded"),
								TEXT("The blocking trace hit is outside the active mesh selection targets."), TEXT("/trace_result"),
								ViewportTraceFact(Ray, &Input.TraceQuery.GetValue(), &FilterHit), ViewportStrings(TargetRefs), Binding.Identity);
							OutError.ActualValue->AsObject()->SetField(TEXT("hit_component_type"), ViewportObjectType(FilterHit.GetComponent()));
							return false;
						}
					}
					FGeometrySelectionUpdateConfig Config;
					Config.ChangeType = Change;
					const FRay3d WorldRay(Ray.Origin, Ray.Direction);
					FInputRayHit RayHit;
					if (!Manager->RayHitTest(WorldRay, RayHit))
					{
						return ViewportFailure(OutError, TEXT("world_spatial_mesh_selection_ray_missed"),
							TEXT("The modeling selection manager found no selectable mesh element along the ray."), TEXT("/selection_ray"),
							ViewportTraceFact(Ray, Input.TraceQuery.IsSet() ? &Input.TraceQuery.GetValue() : nullptr),
							MakeShared<FJsonValueString>(TEXT("selectable mesh element hit")), Binding.Identity);
					}
					if (Input.TraceQuery.IsSet()
						&& RayHit.HitDepth > Input.TraceQuery->MaxDistanceCm)
					{
						return ViewportFailure(OutError, TEXT("world_spatial_mesh_selection_beyond_max_distance"),
							TEXT("The mesh element hit is beyond the maximum selection distance."), TEXT("/trace_query/max_distance_cm"),
							ViewportSpatialFact({RayHit.HitDepth}, TEXT("unreal.world"), TEXT("centimeter")),
							ViewportSpatialFact({Input.TraceQuery->MaxDistanceCm}, TEXT("unreal.world"), TEXT("centimeter")), Binding.Identity);
					}
					FGeometrySelectionUpdateResult Update;
					OutResult = {};
					Manager->UpdateSelectionViaRaycast(
						WorldRay,
						Config,
						Update);
					OutResult = FUeAgentWorldSpatialMutationResult(
						TEXT("world-spatial:") + Context.GetRequestId(),
						TEXT("explicit-modeling-selection-write:") + Binding.Identity);
					return OutResult.TryValidate(OutError);
				}
				return ViewportFailure(OutError, TEXT("world_spatial_viewport_projection_mutation_mismatch"),
					TEXT("The request payload is not supported by viewport selection mutation."), TEXT("/request"),
					MakeShared<FJsonValueString>(Contract.GetCapabilityId()), MakeShared<FJsonValueString>(TEXT("actor or mesh-element selection request")), Binding.Identity);
			}

			bool TrySettle(
				const FUeAgentWorldSpatialExecutionChainContract& Contract,
				const FUeAgentWorldSpatialMutationResult& Mutation,
				const FUeAgentOperationContext& Context,
				FUeAgentWorldSpatialExecutionError& OutError) override
			{
				FViewportBinding Binding;
				return Mutation.TryValidate(OutError)
					&& TryResolveViewport(*Environment, Context, Binding, OutError);
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
				TArray<UE::UAI::Contract::FDiagnostic> Differences;
				ON_SCOPE_EXIT { if (!OutError.IsEmpty()) for (const auto& Difference : Differences) OutError.Append(Difference); };
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
						const FString ExpectedFingerprint = ActorRefSetFingerprint(Expected);
						if (ExpectedFingerprint != AfterFingerprint)
							AddComparisonDifference(Differences, TEXT("viewport_selection.selected_actor_refs"),
								TEXT("The selected actor references differ from the requested selection operation."),
								MakeShared<FJsonValueString>(ExpectedFingerprint), MakeShared<FJsonValueString>(AfterFingerprint));
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
					if (BeforeRef != AfterRef)
						AddComparisonDifference(Differences, TEXT("viewport_selection.hit_actor_ref"), TEXT("The ray hit a different actor during selection verification."),
							MakeShared<FJsonValueString>(BeforeRef), MakeShared<FJsonValueString>(AfterRef));
					else if (Mode != TEXT("replace") && Mode != TEXT("add") && Mode != TEXT("remove") && Mode != TEXT("toggle"))
						AddComparisonDifference(Differences, TEXT("viewport_selection.selection_mode"), TEXT("The selection verifier does not support this mode."),
							MakeShared<FJsonValueString>(TEXT("replace, add, remove, or toggle")), MakeShared<FJsonValueString>(Mode));
					else
					{
						const bool bExpected = Mode == TEXT("toggle") ? !bBefore : Mode != TEXT("remove");
						if (bExpected != bAfter)
							AddComparisonDifference(Differences, TEXT("viewport_selection.hit_actor_selected"), TEXT("The hit actor selection state differs from the requested operation."),
								MakeShared<FJsonValueBoolean>(bExpected), MakeShared<FJsonValueBoolean>(bAfter), AfterRef);
					}
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
					const bool bMatches = Input.SelectionMode == TEXT("replace")
						? AfterCount == 1
						: Input.SelectionMode == TEXT("add")
							? AfterCount >= BeforeCount && AfterCount > 0
							: Input.SelectionMode == TEXT("remove")
								&& AfterCount <= BeforeCount;
					if (!bMatches)
					{
						if (Input.SelectionMode == TEXT("replace"))
							AddComparisonDifference(Differences, TEXT("mesh_selection.count"), TEXT("Replace selection must leave exactly one mesh element selected."), ComparisonNumber(1), ComparisonNumber(AfterCount));
						else if (Input.SelectionMode == TEXT("add") || Input.SelectionMode == TEXT("remove"))
							AddComparisonDifference(Differences, TEXT("mesh_selection.count"), Input.SelectionMode == TEXT("add")
								? TEXT("Add selection must leave a positive count at least as large as the previous count.")
								: TEXT("Remove selection must not increase the previous count."), ComparisonNumber(BeforeCount), ComparisonNumber(AfterCount));
						else
							AddComparisonDifference(Differences, TEXT("mesh_selection.selection_mode"), TEXT("The mesh selection verifier does not support this mode."),
								MakeShared<FJsonValueString>(TEXT("replace, add, or remove")), MakeShared<FJsonValueString>(Input.SelectionMode));
					}
				}
				else
				{
					return ViewportFailure(OutError, TEXT("world_spatial_viewport_projection_compare_mismatch"),
						TEXT("Viewport selection verification requires an actor or mesh element selection request."), TEXT("/request"),
						MakeShared<FJsonValueString>(Contract.GetCapabilityId()), MakeShared<FJsonValueString>(TEXT("actor or mesh-element selection request")));
				}
				SetComparisonOutcome(
					Contract,
					MoveTemp(Differences),
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
						return ViewportFailure(OutError, TEXT("world_spatial_viewport_instance_catalog_empty"),
							TEXT("The native environment returned no available viewport references."), TEXT("/viewports"),
							MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("at least one viewport reference")));
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
				if (!TryResolveViewport(*Environment, Context, Binding, OutError,
					Request.GetPayload().IsType<FViewportCameraFrameRequestInput>()))
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
						return ViewportFailure(OutError, TEXT("world_spatial_viewport_warning_identity_mismatch"),
							TEXT("The warning query viewport does not match the bound viewport."), TEXT("/viewport_ref"),
							MakeShared<FJsonValueString>(Ref.GetValue()), MakeShared<FJsonValueString>(Binding.Identity), Binding.Identity);
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
					const FViewportViewModeListRequestInput& Input =
						Request.GetPayload().Get<FViewportViewModeListRequestInput>();
					if (!TryMakeViewModeCatalog(Input.Query, Context, Modes, OutError)
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
							+ ViewportViewModeId(*Binding.Client)
							+ TEXT("|") + Input.Query,
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
				const FUeAgentWorldSpatialPreflightFence& Fence,
				const FUeAgentOperationContext& Context,
				FUeAgentWorldSpatialMutationResult& OutResult,
				FUeAgentWorldSpatialExecutionError& OutError) override
			{
				OutResult = FUeAgentWorldSpatialMutationResult::NoMutation();
				FViewportBinding Binding;
				if (!TryResolveViewport(*Environment, Context, Binding, OutError,
					Request.GetPayload().IsType<FViewportCameraFrameRequestInput>()))
				{
					return false;
				}
				FString Evidence;
				if (Request.GetPayload().IsType<FViewportCameraFrameRequestInput>())
				{
					if (!TryFrameCamera(
							*Environment,
							*SpaceCompiler,
							Request.GetPayload().Get<FViewportCameraFrameRequestInput>(),
							Binding,
							OutResult, OutError))
					{
						return false;
					}
					Evidence = TEXT("same-viewport-camera-frame:") + Binding.Identity;
				}
				else if (Request.GetPayload().IsType<FViewportStateRestoreRequestInput>())
				{
					const FWorldSpatialViewportState& State = Request.GetPayload()
						.Get<FViewportStateRestoreRequestInput>().Snapshot;
					if (State.ViewportRef != Binding.Identity)
					{
						return ViewportFailure(OutError, TEXT("world_spatial_viewport_restore_identity_mismatch"),
							TEXT("The viewport snapshot belongs to a different viewport."), TEXT("/snapshot/viewport_ref"),
							MakeShared<FJsonValueString>(State.ViewportRef), MakeShared<FJsonValueString>(Binding.Identity), Binding.Identity);
					}
					if (!TryApplyViewportState(
							*SpaceCompiler, MakePatch(State), Binding, OutResult, OutError))
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
							OutResult, OutError))
					{
						return false;
					}
					Evidence = TEXT("same-viewport-state-update:") + Binding.Identity;
				}
				else
				{
					return ViewportFailure(OutError, TEXT("world_spatial_viewport_session_mutation_mismatch"),
						TEXT("The request payload is not supported by viewport state mutation."), TEXT("/request"),
						MakeShared<FJsonValueString>(Contract.GetCapabilityId()), MakeShared<FJsonValueString>(TEXT("camera-frame, state-restore, or state-update request")), Binding.Identity);
				}
				OutResult = FUeAgentWorldSpatialMutationResult(
					TEXT("world-spatial:") + Context.GetRequestId(), Evidence);
				return OutResult.TryValidate(OutError);
			}

			bool TrySettle(
				const FUeAgentWorldSpatialExecutionChainContract& Contract,
				const FUeAgentWorldSpatialMutationResult& Mutation,
				const FUeAgentOperationContext& Context,
				FUeAgentWorldSpatialExecutionError& OutError) override
			{
				FViewportBinding Binding;
				return Mutation.TryValidate(OutError)
					&& TryResolveViewport(*Environment, Context, Binding, OutError, false)
					&& MakeCameraTransform(Binding).TryValidate(OutError);
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
				if (!TryResolveViewport(*Environment, Context, Binding, OutError,
					Request.GetPayload().IsType<FViewportCameraFrameRequestInput>()))
				{
					return false;
				}
				TArray<UE::UAI::Contract::FDiagnostic> Differences;
				ON_SCOPE_EXIT { if (!OutError.IsEmpty()) for (const auto& Difference : Differences) OutError.Append(Difference); };
				const auto AddDifference = [&Differences, &Binding](
					const FString& Path,
					TSharedPtr<FJsonValue> Expected,
					TSharedPtr<FJsonValue> Actual) -> UE::UAI::Contract::FDiagnostic&
				{
					return AddComparisonDifference(Differences, Path, TEXT("Viewport state differs from the requested value."),
						MoveTemp(Expected), MoveTemp(Actual), Binding.Identity);
				};
				const FString ComparisonEvidence =
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
							*Environment, *SpaceCompiler, Input, Binding, Bounds, Actors, OutError)
						|| !TryBuildSceneView(Binding, ViewFamily, View, OutError))
					{
						return false;
					}
					const FIntPoint Size = Binding.Viewport->GetSizeXY();
					const double MarginX = Input.FramePolicy.PaddingRatio * Size.X * 0.5;
					const double MarginY = Input.FramePolicy.PaddingRatio * Size.Y * 0.5;
					for (int32 Corner = 0; Corner < 8; ++Corner)
					{
						const FVector Point(
							(Corner & 1) ? Bounds.Max.X : Bounds.Min.X,
							(Corner & 2) ? Bounds.Max.Y : Bounds.Min.Y,
							(Corner & 4) ? Bounds.Max.Z : Bounds.Min.Z);
						FVector2D Pixel;
						const FString Path = FString::Printf(TEXT("viewport_frame.bounds.corners[%d]"), Corner);
						if (!View->WorldToPixel(Point, Pixel))
						{
							AddComparisonDifference(Differences, Path + TEXT(".projection"), TEXT("The world-space bounds corner could not be projected into the viewport."),
								MakeShared<FJsonValueString>(TEXT("projectable world-space point")), ComparisonComponents({Point.X, Point.Y, Point.Z}), Binding.Identity);
							continue;
						}
						if (!(Pixel.X >= MarginX && Pixel.X <= Size.X - MarginX))
						{
							AddComparisonDifference(Differences, Path + TEXT(".pixel_x"), TEXT("The projected corner is outside the horizontal padded viewport bounds."),
								ComparisonComponents({MarginX, Size.X - MarginX}), ComparisonNumber(Pixel.X), Binding.Identity);
						}
						else if (!(Pixel.Y >= MarginY && Pixel.Y <= Size.Y - MarginY))
						{
							AddComparisonDifference(Differences, Path + TEXT(".pixel_y"), TEXT("The projected corner is outside the vertical padded viewport bounds."),
								ComparisonComponents({MarginY, Size.Y - MarginY}), ComparisonNumber(Pixel.Y), Binding.Identity);
						}
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
						return ViewportFailure(OutError, TEXT("world_spatial_viewport_session_compare_mismatch"),
							TEXT("Viewport state verification requires a state restore or update request."), TEXT("/request"),
							MakeShared<FJsonValueString>(Contract.GetCapabilityId()), MakeShared<FJsonValueString>(TEXT("state-restore or state-update request")), Binding.Identity);
					}
					if (Expected.CameraTransform.IsSet())
					{
						FUeAgentWorldSpatialCompiledValue ExpectedCamera;
						FUeAgentWorldSpatialCompiledValue BeforeCamera;
						FUeAgentWorldSpatialCompiledValue ActualCamera;
						const FWorldSpatialValue& Transform =
							Expected.CameraTransform.GetValue();
						const bool bLocalDelta =
							Expected.CameraTransformMode.GetValue() == TEXT("delta")
							&& Transform.Frame.FrameId == TEXT("viewport.camera.local");
						if (!SpaceCompiler->TryConvertToNative(
								Transform,
								bLocalDelta
									? TEXT("viewport.camera.local") : TEXT("unreal.world"),
								TEXT("centimeter"),
								ExpectedCamera,
								OutError)
							|| !TryReadCompiledValue(
								Before,
								TEXT("viewport_state.camera_transform"),
								BeforeCamera,
								OutError)
							|| !TryReadCompiledValue(
								After,
								TEXT("viewport_state.camera_transform"),
								ActualCamera,
								OutError))
						{
							return false;
						}
						if (Expected.CameraTransformMode.GetValue() == TEXT("delta"))
						{
							const FVector BeforeLocation(
								BeforeCamera.Components[0], BeforeCamera.Components[1],
								BeforeCamera.Components[2]);
							const FQuat BeforeRotation(
								BeforeCamera.Components[3], BeforeCamera.Components[4],
								BeforeCamera.Components[5], BeforeCamera.Components[6]);
							const FVector DeltaLocation(
								ExpectedCamera.Components[0], ExpectedCamera.Components[1],
								ExpectedCamera.Components[2]);
							const FQuat DeltaRotation(
								ExpectedCamera.Components[3], ExpectedCamera.Components[4],
								ExpectedCamera.Components[5], ExpectedCamera.Components[6]);
							const FVector ComposedLocation = BeforeLocation
								+ (bLocalDelta
									? BeforeRotation.RotateVector(DeltaLocation)
									: DeltaLocation);
							const FQuat ComposedRotation = bLocalDelta
								? BeforeRotation * DeltaRotation
								: DeltaRotation * BeforeRotation;
							ExpectedCamera.Components = {
								ComposedLocation.X, ComposedLocation.Y, ComposedLocation.Z,
								ComposedRotation.X, ComposedRotation.Y,
								ComposedRotation.Z, ComposedRotation.W,
								1.0, 1.0, 1.0};
							ExpectedCamera.FrameId = TEXT("unreal.world");
						}
						CompareTransforms(ExpectedCamera, ActualCamera, Differences,
							TEXT("viewport_state.camera_transform"), Binding.Identity, 0.001);
					}
					if (Expected.FovDegrees.IsSet())
					{
						double Actual = 0.0;
						if (!TryReadDecimalValue(
								After, TEXT("viewport_state.fov"), Actual, OutError))
						{
							return false;
						}
						if (!FMath::IsNearlyEqual(Expected.FovDegrees.GetValue(), Actual, 0.001))
							AddDifference(TEXT("viewport_state.fov"),
								MakeShared<FJsonValueNumber>(Expected.FovDegrees.GetValue()),
								MakeShared<FJsonValueNumber>(Actual));
					}
					if (Expected.ViewMode.IsSet())
					{
						FString Actual;
						if (!TryReadStringValue(
								After, TEXT("viewport_state.view_mode"), Actual, OutError))
						{
							return false;
						}
						UeAgentViewportViewMode::FResolvedViewportViewMode RequestedMode;
						if (!TryResolveViewModeId(Expected.ViewMode.GetValue(), RequestedMode, OutError))
						{
							return false;
						}
						if (!UeAgentViewportViewMode::DoesViewportViewModeMatch(RequestedMode,
							UeAgentViewportViewMode::CaptureViewportViewModeState(Binding.Client)))
							AddDifference(TEXT("viewport_state.view_mode"),
								MakeShared<FJsonValueString>(Expected.ViewMode.GetValue()),
								MakeShared<FJsonValueString>(Actual));
					}
					if (Expected.Realtime.IsSet())
					{
						bool Actual = false;
						if (!TryReadBooleanValue(
								After, TEXT("viewport_state.realtime"), Actual, OutError))
						{
							return false;
						}
						FString OverrideReason;
						if (!TryReadStringValue(After, TEXT("viewport_state.realtime_override_reason"), OverrideReason, OutError))
						{
							return false;
						}
						if (Expected.Realtime.GetValue() != Actual)
						{
							auto& Difference = AddDifference(TEXT("viewport_state.realtime"),
								MakeShared<FJsonValueBoolean>(Expected.Realtime.GetValue()),
								MakeShared<FJsonValueBoolean>(Actual));
							if (!OverrideReason.IsEmpty())
							{
								Difference.Code = TEXT("world_spatial_viewport_realtime_overridden");
								Difference.PublicMessage = FString::Printf(
									TEXT("Editor realtime override prevents the requested effective state: %s"),
									*OverrideReason);
								Difference.SuggestedFix = TEXT("Allow the editor override to end before requesting effective realtime again.");
							}
						}
					}
					if (Expected.GameView.IsSet())
					{
						bool Actual = false;
						if (!TryReadBooleanValue(
								After, TEXT("viewport_state.game_view"), Actual, OutError))
						{
							return false;
						}
						if (Expected.GameView.GetValue() != Actual)
							AddDifference(TEXT("viewport_state.game_view"),
								MakeShared<FJsonValueBoolean>(Expected.GameView.GetValue()),
								MakeShared<FJsonValueBoolean>(Actual));
					}
					if (Expected.ShowFlags.IsSet())
					{
						const FUeAgentStateEntry* Entry = FindEntryByRole(
							After, TEXT("viewport_state.show_flags"));
						if (!Entry)
						{
							return ViewportReadFailure(OutError, TEXT("world_spatial_viewport_show_flags_readback_invalid"),
								TEXT("is missing from the snapshot."), TEXT("viewport_state.show_flags"), TEXT("/entries"),
								nullptr, MakeShared<FJsonValueString>(TEXT("show_flags observation")));
						}
						if (Entry->GetValue()->GetKind() != EUeAgentValueKind::Array)
						{
							return ViewportReadFailure(OutError, TEXT("world_spatial_viewport_show_flags_readback_invalid"),
								TEXT("must contain an array."), TEXT("viewport_state.show_flags"), TEXT("/value"),
								CanonicalFieldFact(Entry->GetValue()), MakeShared<FJsonValueString>(TEXT("array")));
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
						if (Actual != Wanted)
						{
							const auto ToStringsValue = [](const TArray<FString>& Values)
							{
								TArray<TSharedPtr<FJsonValue>> Items;
								for (const FString& Value : Values)
									Items.Add(MakeShared<FJsonValueString>(Value));
								return MakeShared<FJsonValueArray>(MoveTemp(Items));
							};
							AddDifference(TEXT("viewport_state.show_flags"),
								ToStringsValue(Wanted), ToStringsValue(Actual));
						}
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
						if (Actual != Wanted.bModeWidgets)
							AddDifference(TEXT("viewport_state.interaction.mode_widgets"),
								MakeShared<FJsonValueBoolean>(Wanted.bModeWidgets), MakeShared<FJsonValueBoolean>(Actual));
						if (!TryReadBooleanValue(After,
								TEXT("viewport_state.interaction.selection"), Actual, OutError))
						{
							return false;
						}
						if (Actual != Wanted.bSelection)
							AddDifference(TEXT("viewport_state.interaction.selection"),
								MakeShared<FJsonValueBoolean>(Wanted.bSelection), MakeShared<FJsonValueBoolean>(Actual));
						if (!TryReadBooleanValue(After,
								TEXT("viewport_state.interaction.pivot"), Actual, OutError))
						{
							return false;
						}
						if (Actual != Wanted.bPivot)
							AddDifference(TEXT("viewport_state.interaction.pivot"),
								MakeShared<FJsonValueBoolean>(Wanted.bPivot), MakeShared<FJsonValueBoolean>(Actual));
						if (!TryReadBooleanValue(After,
								TEXT("viewport_state.interaction.editor_primitives"), Actual, OutError))
						{
							return false;
						}
						if (Actual != Wanted.bEditorPrimitives)
							AddDifference(TEXT("viewport_state.interaction.editor_primitives"),
								MakeShared<FJsonValueBoolean>(Wanted.bEditorPrimitives), MakeShared<FJsonValueBoolean>(Actual));
						if (!TryReadBooleanValue(After,
								TEXT("viewport_state.interaction.game_primitives"), Actual, OutError))
						{
							return false;
						}
						if (Actual != Wanted.bGamePrimitives)
							AddDifference(TEXT("viewport_state.interaction.game_primitives"),
								MakeShared<FJsonValueBoolean>(Wanted.bGamePrimitives), MakeShared<FJsonValueBoolean>(Actual));
						if (!TryReadStringValue(After,
								TEXT("viewport_state.interaction.widget_mode"),
								ActualWidgetMode, OutError))
						{
							return false;
						}
						if (ActualWidgetMode != Wanted.WidgetMode)
							AddDifference(TEXT("viewport_state.interaction.widget_mode"),
								MakeShared<FJsonValueString>(Wanted.WidgetMode), MakeShared<FJsonValueString>(ActualWidgetMode));
					}
				}
				SetComparisonOutcome(
					Contract,
					MoveTemp(Differences),
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
