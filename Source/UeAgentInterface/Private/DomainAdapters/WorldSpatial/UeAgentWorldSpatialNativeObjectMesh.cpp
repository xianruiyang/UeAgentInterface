#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialNativeInternal.h"


#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "MeshDescription.h"
#include "Misc/ScopeExit.h"
#include "StaticMeshAttributes.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"
#include <initializer_list>

namespace UE::UAI::DomainAdapters::WorldSpatial::Private
{
	using namespace StateSemantic;

	namespace
	{
		bool MeshFailure(FUeAgentWorldSpatialExecutionError& OutError, const TCHAR* Code, const TCHAR* Reason,
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

		TSharedPtr<FJsonValue> MeshObjectType(const UObject* Object)
		{
			return Object ? MakeShared<FJsonValueString>(Object->GetClass()->GetPathName()) : TSharedPtr<FJsonValue>();
		}

		TSharedPtr<FJsonValue> MeshChoices(std::initializer_list<const TCHAR*> Values)
		{
			TArray<TSharedPtr<FJsonValue>> Items;
			for (const TCHAR* Value : Values) Items.Add(MakeShared<FJsonValueString>(Value));
			return MakeShared<FJsonValueArray>(MoveTemp(Items));
		}

		TSharedPtr<FJsonValue> MeshSpatialFact(const TArray<double>& Components, const FString& Frame,
			const FString& Unit, const TCHAR* Representation = nullptr)
		{
			const TSharedRef<FJsonObject> Fact = MakeShared<FJsonObject>();
			Fact->SetStringField(TEXT("frame_id"), Frame);
			Fact->SetStringField(TEXT("unit"), Unit);
			Fact->SetField(TEXT("components"), ComparisonComponents(Components));
			if (Representation) Fact->SetStringField(TEXT("representation"), Representation);
			return MakeShared<FJsonValueObject>(Fact);
		}

		FUeAgentWorldSpatialCompiledValue MakeTransformValue(
			const FString& Subject,
			const FTransform& Transform,
			const FString& Evidence)
		{
			const FVector Location = Transform.GetLocation();
			const FQuat Rotation = Transform.GetRotation();
			const FVector Scale = Transform.GetScale3D();
			return {
				Subject,
				TEXT("transform"),
				TEXT("unreal.world"),
				3,
				TEXT("centimeter"),
				{
					Location.X, Location.Y, Location.Z,
					Rotation.X, Rotation.Y, Rotation.Z, Rotation.W,
					Scale.X, Scale.Y, Scale.Z,
				},
				Evidence,
			};
		}

		FUeAgentWorldSpatialCompiledValue MakePointValue(
			const FString& Subject,
			const FString& FrameId,
			const FString& Unit,
			const FVector& Point,
			const FString& Evidence)
		{
			return {
				Subject,
				TEXT("point"),
				FrameId,
				3,
				Unit,
				{Point.X, Point.Y, Point.Z},
				Evidence,
			};
		}

		bool TryParseWorldValue(
			const FWorldSpatialValue& Value,
			FUeAgentWorldSpatialCompiledValue& OutValue,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			OutValue = {};
			TArray<double> Components;
			for (int32 Index = 0; Index < Value.Components.Num(); ++Index)
			{
				const FString& Lexeme = Value.Components[Index];
				double Parsed = 0.0;
				if (!LexTryParseString(Parsed, *Lexeme))
				{
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("world_spatial_compiled_number_invalid"),
						TEXT("The spatial component cannot be parsed as a native number."), TEXT("native_execution"));
					OutError.TargetPath = Value.Subject;
					OutError.PropertyPath = FString::Printf(TEXT("/components/%d"), Index);
					OutError.SourceKind = TEXT("world_spatial_value");
					OutError.Source.JsonPath = OutError.PropertyPath;
					OutError.ActualValue = MakeShared<FJsonValueString>(Lexeme);
					OutError.ActualValuePresent = true;
					OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("number"));
					return false;
				}
				Components.Add(Parsed);
			}
			OutValue = {
				Value.Subject,
				Value.Quantity,
				Value.Frame.FrameId,
				Value.Frame.Dimension,
				Value.Frame.Unit,
				MoveTemp(Components),
				Value.ConversionEvidenceIds.IsEmpty()
					? Value.Frame.BasisEvidenceId
					: Value.ConversionEvidenceIds.Last(),
			};
			return OutValue.TryValidate(OutError);
		}

		bool TryResolveActor(
			IUeAgentWorldSpatialNativeEnvironment& Environment,
			const FString& Ref,
			AActor*& OutActor,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			UObject* Object = nullptr;
			if (!Environment.TryResolveObject(Ref, Object, OutError))
			{
				return false;
			}
			OutActor = Cast<AActor>(Object);
			return OutActor
				? true
				: MeshFailure(OutError, TEXT("world_spatial_actor_type_mismatch"), TEXT("The reference resolves to an object that is not an Actor."),
					TEXT("/object_ref"), MeshObjectType(Object), MakeShared<FJsonValueString>(TEXT("AActor")), Ref);
		}

		bool TryResolveMesh(
			IUeAgentWorldSpatialNativeEnvironment& Environment,
			const FString& Ref,
			UStaticMesh*& OutMesh,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			UObject* Object = nullptr;
			if (!Environment.TryResolveObject(Ref, Object, OutError))
			{
				return false;
			}
			OutMesh = Cast<UStaticMesh>(Object);
			return OutMesh
				? true
				: MeshFailure(OutError, TEXT("world_spatial_mesh_type_mismatch"), TEXT("The reference resolves to an object that is not a static mesh."),
					TEXT("/mesh_ref"), MeshObjectType(Object), MakeShared<FJsonValueString>(TEXT("UStaticMesh")), Ref);
		}

		ECollisionChannel TraceChannel(const FString& Id, bool& bValid)
		{
			bValid = true;
			if (Id == TEXT("visibility"))
			{
				return ECC_Visibility;
			}
			if (Id == TEXT("camera"))
			{
				return ECC_Camera;
			}
			if (Id == TEXT("world_static"))
			{
				return ECC_WorldStatic;
			}
			bValid = false;
			return ECC_Visibility;
		}

		bool TryTraceSurface(
			IUeAgentWorldSpatialNativeEnvironment& Environment,
			IUeAgentWorldSpatialSpaceCompiler& Compiler,
			const FWorldObjectSnapToSurfaceRequestInput& Input,
			FHitResult& OutHit,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			FUeAgentWorldSpatialCompiledValue Origin;
			FUeAgentWorldSpatialCompiledValue Direction;
			if (!Compiler.TryConvertToNative(
					Input.SurfaceQuery.Origin,
					TEXT("unreal.world"),
					TEXT("centimeter"),
					Origin,
					OutError)
				|| !Compiler.TryConvertToNative(
					Input.SurfaceQuery.Direction,
					TEXT("unreal.world"),
					TEXT("unitless"),
					Direction,
					OutError))
			{
				return false;
			}
			const FVector Start(
				Origin.Components[0], Origin.Components[1], Origin.Components[2]);
			const FVector DirectionVector(
				Direction.Components[0], Direction.Components[1], Direction.Components[2]);
			if (DirectionVector.IsNearlyZero())
			{
				return MeshFailure(OutError, TEXT("world_spatial_surface_direction_zero"),
					TEXT("The surface trace direction is zero within the engine's near-zero tolerance."), TEXT("/surface_query/direction"),
					MeshSpatialFact(Direction.Components, Direction.FrameId, Direction.Unit),
					MakeShared<FJsonValueString>(TEXT("non-zero direction")), Input.SurfaceQuery.Direction.Subject);
			}
			UWorld* World = nullptr;
			FCollisionQueryParams Params(
				TEXT("UAIWorldSpatialSurface"),
				Input.SurfaceQuery.bTraceComplex);
			for (const FString& Ref : Input.SurfaceQuery.IgnoreTargetRefs)
			{
				AActor* Actor = nullptr;
				if (!TryResolveActor(Environment, Ref, Actor, OutError))
				{
					return false;
				}
				Params.AddIgnoredActor(Actor);
				World = World ? World : Actor->GetWorld();
			}
			if (!World && !Input.SubjectRefs.IsEmpty())
			{
				AActor* Actor = nullptr;
				if (!TryResolveActor(
						Environment, Input.SubjectRefs[0], Actor, OutError))
				{
					return false;
				}
				World = Actor->GetWorld();
			}
			bool bChannelValid = false;
			const ECollisionChannel Channel = TraceChannel(
				Input.SurfaceQuery.TraceChannel, bChannelValid);
			if (!World)
				return MeshFailure(OutError, TEXT("world_spatial_surface_trace_context_invalid"),
					TEXT("No trace world could be resolved from the ignored Actors or subject Actors."), TEXT("/surface_query/world"),
					nullptr, MakeShared<FJsonValueString>(TEXT("available Actor world")), Input.SurfaceQuery.Origin.Subject);
			if (!bChannelValid)
				return MeshFailure(OutError, TEXT("world_spatial_surface_trace_context_invalid"),
					TEXT("The surface trace channel is not supported."), TEXT("/surface_query/trace_channel"),
					MakeShared<FJsonValueString>(Input.SurfaceQuery.TraceChannel),
					MeshChoices({TEXT("visibility"), TEXT("camera"), TEXT("world_static")}), Input.SurfaceQuery.Origin.Subject);
			const FVector End = Start
				+ DirectionVector.GetSafeNormal()
					* Input.SurfaceQuery.MaxDistanceCm;
			if (World->LineTraceSingleByChannel(OutHit, Start, End, Channel, Params)) return true;
			const TSharedRef<FJsonObject> Trace = MakeShared<FJsonObject>();
			Trace->SetField(TEXT("origin"), MeshSpatialFact(Origin.Components, Origin.FrameId, Origin.Unit));
			Trace->SetField(TEXT("direction"), MeshSpatialFact(Direction.Components, Direction.FrameId, Direction.Unit));
			Trace->SetField(TEXT("max_distance_cm"), ComparisonNumber(Input.SurfaceQuery.MaxDistanceCm));
			Trace->SetStringField(TEXT("trace_channel"), Input.SurfaceQuery.TraceChannel);
			Trace->SetBoolField(TEXT("trace_complex"), Input.SurfaceQuery.bTraceComplex);
			Trace->SetNumberField(TEXT("ignored_actor_count"), Input.SurfaceQuery.IgnoreTargetRefs.Num());
			return MeshFailure(OutError, TEXT("world_spatial_surface_hit_missing"),
				TEXT("The surface trace found no blocking hit with the requested ray and collision settings."), TEXT("/surface_query"),
				MakeShared<FJsonValueObject>(Trace), MakeShared<FJsonValueString>(TEXT("blocking surface hit")), Input.SurfaceQuery.Origin.Subject);
		}

		void FindMeshComponents(
			UStaticMesh* Mesh,
			TArray<UStaticMeshComponent*>& OutComponents)
		{
			OutComponents.Reset();
			for (TObjectIterator<UStaticMeshComponent> It; It; ++It)
			{
				UStaticMeshComponent* Component = *It;
				if (IsValid(Component)
					&& !Component->HasAnyFlags(RF_ClassDefaultObject)
					&& Component->GetStaticMesh() == Mesh)
				{
					OutComponents.Add(Component);
				}
			}
			OutComponents.Sort(
				[](const UStaticMeshComponent& Left, const UStaticMeshComponent& Right)
				{
					return Left.GetPathName() < Right.GetPathName();
				});
		}

		bool TryAxisIndex(
			const FString& Axis,
			int32& OutIndex,
			FUeAgentWorldSpatialExecutionError& OutError,
			const FString& Target)
		{
			if (Axis == TEXT("x")) OutIndex = 0;
			else if (Axis == TEXT("y")) OutIndex = 1;
			else if (Axis == TEXT("z")) OutIndex = 2;
			else return MeshFailure(OutError, TEXT("world_spatial_alignment_axis_invalid"),
				TEXT("The alignment axis is not supported."), TEXT("/alignment/axis"), MakeShared<FJsonValueString>(Axis),
				MeshChoices({TEXT("x"), TEXT("y"), TEXT("z")}), Target);
			return true;
		}

		double AxisValue(const FVector& Value, int32 AxisIndex)
		{
			return AxisIndex == 0 ? Value.X : AxisIndex == 1 ? Value.Y : Value.Z;
		}

		FVector AxisDelta(int32 AxisIndex, double Amount)
		{
			return AxisIndex == 0 ? FVector(Amount, 0.0, 0.0)
				: AxisIndex == 1 ? FVector(0.0, Amount, 0.0)
				: FVector(0.0, 0.0, Amount);
		}

		bool TryBoundsAnchorValue(
			const FBox& Bounds,
			int32 AxisIndex,
			const FString& Anchor,
			double& OutValue,
			FUeAgentWorldSpatialExecutionError& OutError,
			const FString& Path,
			const FString& Target)
		{
			if (!Bounds.IsValid)
			{
				return MeshFailure(OutError, TEXT("world_spatial_alignment_bounds_invalid"),
					TEXT("The Actor has no valid component bounds for alignment."), TEXT("/bounds"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Target);
			}
			if (Anchor == TEXT("min")) OutValue = AxisValue(Bounds.Min, AxisIndex);
			else if (Anchor == TEXT("max")) OutValue = AxisValue(Bounds.Max, AxisIndex);
			else if (Anchor == TEXT("center")) OutValue = AxisValue(Bounds.GetCenter(), AxisIndex);
			else return MeshFailure(OutError, TEXT("world_spatial_alignment_anchor_invalid"),
				TEXT("The bounds anchor is not supported."), Path, MakeShared<FJsonValueString>(Anchor),
				MeshChoices({TEXT("min"), TEXT("max"), TEXT("center")}), Target);
			return true;
		}

		bool TryResolveFace(
			const FString& Face,
			const FString& FallbackAxis,
			int32& OutAxisIndex,
			FString& OutAnchor,
			FUeAgentWorldSpatialExecutionError& OutError,
			const FString& Path,
			const FString& Target)
		{
			const FString Normalized = Face.ToLower();
			if (Normalized == TEXT("min") || Normalized == TEXT("center") || Normalized == TEXT("max"))
			{
				OutAnchor = Normalized;
				return TryAxisIndex(FallbackAxis, OutAxisIndex, OutError, Target);
			}
			struct FFaceRow { const TCHAR* Id; int32 Axis; const TCHAR* Anchor; };
			static const FFaceRow Rows[] = {
				{TEXT("-x"), 0, TEXT("min")}, {TEXT("x-"), 0, TEXT("min")}, {TEXT("negx"), 0, TEXT("min")},
				{TEXT("+x"), 0, TEXT("max")}, {TEXT("x+"), 0, TEXT("max")}, {TEXT("posx"), 0, TEXT("max")},
				{TEXT("-y"), 1, TEXT("min")}, {TEXT("y-"), 1, TEXT("min")}, {TEXT("negy"), 1, TEXT("min")},
				{TEXT("+y"), 1, TEXT("max")}, {TEXT("y+"), 1, TEXT("max")}, {TEXT("posy"), 1, TEXT("max")},
				{TEXT("-z"), 2, TEXT("min")}, {TEXT("z-"), 2, TEXT("min")}, {TEXT("negz"), 2, TEXT("min")},
				{TEXT("+z"), 2, TEXT("max")}, {TEXT("z+"), 2, TEXT("max")}, {TEXT("posz"), 2, TEXT("max")},
			};
			for (const FFaceRow& Row : Rows)
			{
				if (Normalized == Row.Id)
				{
					OutAxisIndex = Row.Axis;
					OutAnchor = Row.Anchor;
					return true;
				}
			}
			TArray<TSharedPtr<FJsonValue>> Choices = {MakeShared<FJsonValueString>(TEXT("min")),
				MakeShared<FJsonValueString>(TEXT("center")), MakeShared<FJsonValueString>(TEXT("max"))};
			for (const FFaceRow& Row : Rows) Choices.Add(MakeShared<FJsonValueString>(Row.Id));
			return MeshFailure(OutError, TEXT("world_spatial_alignment_face_invalid"),
				TEXT("The alignment face is not supported (face names are case-insensitive)."), Path,
				MakeShared<FJsonValueString>(Face), MakeShared<FJsonValueArray>(MoveTemp(Choices)), Target);
		}

		bool TryActorWorldVertices(
			AActor& Actor,
			TArray<FVector>& OutVertices,
			FUeAgentWorldSpatialExecutionError& OutError,
			const FString& ActorRef)
		{
			OutVertices.Reset();
			UStaticMeshComponent* Component = Actor.FindComponentByClass<UStaticMeshComponent>();
			UStaticMesh* Mesh = Component ? Component->GetStaticMesh() : nullptr;
			const FMeshDescription* Description = Mesh ? Mesh->GetMeshDescription(0) : nullptr;
			if (!Component || !Description || Description->Vertices().Num() == 0)
			{
				return MeshFailure(OutError, TEXT("world_spatial_alignment_mesh_vertices_missing"),
					!Component ? TEXT("The Actor has no static mesh component for vertex alignment.")
						: !Mesh ? TEXT("The Actor's static mesh component has no mesh.")
						: !Description ? TEXT("The Actor's mesh has no LOD 0 mesh description.") : TEXT("The Actor's LOD 0 mesh has no vertices."),
					!Component ? TEXT("/mesh_component") : !Mesh ? TEXT("/mesh_component/mesh")
						: !Description ? TEXT("/lods/0/mesh_description") : TEXT("/lods/0/vertices"),
					Description ? MakeShared<FJsonValueNumber>(0) : TSharedPtr<FJsonValue>(),
					MakeShared<FJsonValueString>(TEXT("static mesh component with non-empty LOD 0 vertices")), ActorRef);
			}
			FStaticMeshConstAttributes Attributes(*Description);
			const TVertexAttributesConstRef<FVector3f> Positions = Attributes.GetVertexPositions();
			for (const FVertexID VertexId : Description->Vertices().GetElementIDs())
			{
				OutVertices.Add(Component->GetComponentTransform().TransformPosition(FVector(Positions[VertexId])));
			}
			if (OutVertices.IsEmpty())
				return MeshFailure(OutError, TEXT("world_spatial_alignment_mesh_vertices_missing"),
					TEXT("The mesh returned no vertex elements to transform into world space."), TEXT("/lods/0/vertices"),
					MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueNumber>(Description->Vertices().Num()), ActorRef);
			return true;
		}

		class FObjectTransformNativeFamily final : public IWorldSpatialNativeFamily
		{
		public:
			explicit FObjectTransformNativeFamily(
				const FUeAgentWorldSpatialNativeDependencies& InDependencies)
				: Environment(InDependencies.Environment.ToSharedRef())
				, SpaceCompiler(InDependencies.SpaceCompiler.ToSharedRef())
				, RevisionSource(InDependencies.RevisionSource.ToSharedRef())
			{
			}

			const FString& GetFamilyId() const override
			{
				static const FString Id = TEXT("object_transform");
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
				if (Request.GetPayload().IsType<FMeshTransformBakeRequestInput>())
				{
					return TryReadMesh(
						Contract,
						Request.GetPayload().Get<FMeshTransformBakeRequestInput>().Transform.Subject,
						Phase,
						Context,
						OutSnapshot,
						OutError,
						true);
				}
				if (Request.GetPayload().IsType<FMeshPivotUpdateRequestInput>())
				{
					return TryReadMesh(
						Contract,
						Request.GetPayload().Get<FMeshPivotUpdateRequestInput>().Pivot.Subject,
						Phase,
						Context,
						OutSnapshot,
						OutError,
						true);
				}

				TArray<FString> SubjectRefs;
				const FWorldSpatialFrame* QueryFrame = nullptr;
				const FWorldObjectSnapToSurfaceRequestInput* Snap = nullptr;
				if (Request.GetPayload().IsType<FObjectTransformReadRequestInput>())
				{
					if (!Target.IsSet() || Target->GetPublicRef().IsEmpty())
					{
						return MeshFailure(OutError, TEXT("world_spatial_object_query_target_missing"),
							TEXT("Object transform readback requires a bound public target reference."), TEXT("/target"),
							Target.IsSet() ? MakeShared<FJsonValueString>(Target->GetPublicRef()) : TSharedPtr<FJsonValue>(),
							MakeShared<FJsonValueString>(TEXT("non-empty bound target reference")), Contract.GetCapabilityId());
					}
					const FObjectTransformReadRequestInput& Input = Request.GetPayload()
						.Get<FObjectTransformReadRequestInput>();
					UObject* TargetObject = nullptr;
					if (!Environment->TryResolveObject(
							Target->GetPublicRef(), TargetObject, OutError))
					{
						return false;
					}
					if (Cast<UStaticMesh>(TargetObject))
					{
						if (Input.VerifyFrame.FrameId != TEXT("mesh.geometry.local")
							|| (!Input.VerifyFrame.bAuthored && Input.VerifyFrame.BasisFrameId != TEXT("mesh.geometry.local"))
							|| Input.VerifyFrame.Dimension != 3
							|| Input.VerifyFrame.Unit != TEXT("centimeter"))
						{
							const auto MakeFrameFact = [](const FString& Frame, const FString& Basis, int32 Dimension, const FString& Unit)
							{
								const TSharedRef<FJsonObject> Fact = MakeShared<FJsonObject>();
								Fact->SetStringField(TEXT("frame_id"), Frame);
								Fact->SetStringField(TEXT("basis_frame_id"), Basis);
								Fact->SetNumberField(TEXT("dimension"), Dimension);
								Fact->SetStringField(TEXT("unit"), Unit);
								return MakeShared<FJsonValueObject>(Fact);
							};
							return MeshFailure(OutError, TEXT("world_spatial_mesh_query_frame_invalid"),
								TEXT("Static mesh readback requires a three-dimensional mesh geometry frame in centimeters."), TEXT("/verify_frame"),
								MakeFrameFact(Input.VerifyFrame.FrameId, Input.VerifyFrame.BasisFrameId, Input.VerifyFrame.Dimension, Input.VerifyFrame.Unit),
								MakeFrameFact(TEXT("mesh.geometry.local"), TEXT("mesh.geometry.local"), 3, TEXT("centimeter")), Target->GetPublicRef());
						}
						return TryReadMesh(
							Contract,
							Target->GetPublicRef(),
							Phase,
							Context,
							OutSnapshot,
							OutError,
							true);
					}
					if (!Cast<AActor>(TargetObject))
					{
						return MeshFailure(OutError, TEXT("world_spatial_object_query_target_type_unsupported"),
							TEXT("Object transform readback supports Actors and static meshes."), TEXT("/target"),
							MeshObjectType(TargetObject), MeshChoices({TEXT("AActor"), TEXT("UStaticMesh")}), Target->GetPublicRef());
					}
					SubjectRefs.Add(Target->GetPublicRef());
					QueryFrame = &Input.VerifyFrame;
				}
				else if (Request.GetPayload().IsType<FWorldObjectAlignRequestInput>())
				{
					const FWorldObjectAlignRequestInput& Align = Request.GetPayload()
						.Get<FWorldObjectAlignRequestInput>();
					SubjectRefs = Align.SubjectRefs;
					if (Align.AnchorRef.IsSet()
						&& !SubjectRefs.Contains(Align.AnchorRef.GetValue()))
					{
						SubjectRefs.Add(Align.AnchorRef.GetValue());
					}
				}
				else if (Request.GetPayload().IsType<
					FWorldObjectTransformUpdateRequestInput>())
				{
					SubjectRefs.Add(Request.GetPayload()
						.Get<FWorldObjectTransformUpdateRequestInput>()
						.Transform.Subject);
				}
				else if (Request.GetPayload().IsType<FWorldObjectSnapToSurfaceRequestInput>())
				{
					Snap = &Request.GetPayload()
						.Get<FWorldObjectSnapToSurfaceRequestInput>();
					SubjectRefs = Snap->SubjectRefs;
				}
				else
				{
					return MeshFailure(OutError, TEXT("world_spatial_object_read_request_mismatch"),
						TEXT("Object and mesh readback received an unsupported request kind."), TEXT("/request_kind"),
						MakeShared<FJsonValueString>(Request.GetRequestKindId()),
						MeshChoices({TEXT("transform read"), TEXT("transform update"), TEXT("alignment"), TEXT("surface snap"), TEXT("mesh bake"), TEXT("mesh pivot")}), Contract.GetCapabilityId());
				}

				TArray<FObservedEntry> Entries;
				FString Fingerprint;
				for (const FString& SubjectRef : SubjectRefs)
				{
					AActor* Actor = nullptr;
					if (!TryResolveActor(*Environment, SubjectRef, Actor, OutError))
					{
						return false;
					}
					FUeAgentWorldSpatialCompiledValue Value = MakeTransformValue(
						SubjectRef,
						Actor->GetActorTransform(),
						TEXT("actor-transform-independent-readback"));
					if (QueryFrame)
					{
						FWorldSpatialValue Converted;
						if (!SpaceCompiler->TryConvertFromNative(
								Value, *QueryFrame, Converted, OutError)
							|| !TryParseWorldValue(Converted, Value, OutError))
						{
							return false;
						}
					}
					FUeAgentCanonicalValuePtr Canonical;
					if (!TryMakeCompiledValue(Value, Context, Canonical, OutError))
					{
						return false;
					}
					Entries.Add({
						TEXT("world-spatial:actor-transform:") + SubjectRef,
						TEXT("actor"),
						Context.GetExecutionContext().GetWorldIdentity(),
						SubjectRef,
						TEXT("actor_transform:") + SubjectRef,
						TEXT("world_spatial.compiled_value.v1"),
						Canonical,
					});
					for (const double Component : Value.Components)
					{
						Fingerprint += FString::SanitizeFloat(Component, 9) + TEXT("|");
					}
				}

				if (Snap)
				{
					FHitResult Hit;
					if (!TryTraceSurface(
							*Environment, *SpaceCompiler, *Snap, Hit, OutError))
					{
						return false;
					}
					FUeAgentCanonicalValuePtr Point;
					FUeAgentCanonicalValuePtr Normal;
					if (!TryMakeCompiledValue(
							MakePointValue(
								TEXT("surface:hit"),
								TEXT("unreal.world"),
								TEXT("centimeter"),
								Hit.ImpactPoint,
								TEXT("world-line-trace-impact")),
							Context,
							Point,
							OutError)
						|| !TryMakeCompiledValue(
							{
								TEXT("surface:hit"),
								TEXT("normal"),
								TEXT("unreal.world"),
								3,
								TEXT("unitless"),
								{Hit.ImpactNormal.X, Hit.ImpactNormal.Y, Hit.ImpactNormal.Z},
								TEXT("world-line-trace-normal"),
							},
							Context,
							Normal,
							OutError))
					{
						return false;
					}
					Entries.Add({
						TEXT("world-spatial:surface-hit:point"),
						TEXT("surface_hit"),
						Context.GetExecutionContext().GetWorldIdentity(),
						TEXT("surface:hit"),
						TEXT("surface_hit.point"),
						TEXT("world_spatial.compiled_value.v1"),
						Point,
					});
					Entries.Add({
						TEXT("world-spatial:surface-hit:normal"),
						TEXT("surface_hit"),
						Context.GetExecutionContext().GetWorldIdentity(),
						TEXT("surface:hit"),
						TEXT("surface_hit.normal"),
						TEXT("world_spatial.compiled_value.v1"),
						Normal,
					});
					Fingerprint += Hit.ImpactPoint.ToString()
						+ TEXT("|") + Hit.ImpactNormal.ToString();
				}

				return TryMakeSnapshot(
					Contract,
					Phase,
					Fingerprint,
					Entries,
					Context,
					OutSnapshot,
					OutError
				);
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
				bool bMutated = false;
				if (Request.GetPayload().IsType<
					FWorldObjectTransformUpdateRequestInput>())
				{
					bMutated = TryUpdateTransform(
						Request.GetPayload().Get<
							FWorldObjectTransformUpdateRequestInput>(),
						OutResult, OutError);
				}
				else if (Request.GetPayload().IsType<FWorldObjectAlignRequestInput>())
				{
					bMutated = TryAlign(
						Request.GetPayload().Get<FWorldObjectAlignRequestInput>(), OutResult, OutError);
				}
				else if (Request.GetPayload().IsType<FWorldObjectSnapToSurfaceRequestInput>())
				{
					bMutated = TrySnap(
						Request.GetPayload().Get<FWorldObjectSnapToSurfaceRequestInput>(), OutResult, OutError);
				}
				else if (Request.GetPayload().IsType<FMeshTransformBakeRequestInput>())
				{
					bMutated = TryBake(
						Request.GetPayload().Get<FMeshTransformBakeRequestInput>(), OutResult, OutError);
				}
				else if (Request.GetPayload().IsType<FMeshPivotUpdateRequestInput>())
				{
					bMutated = TryPivot(
						Request.GetPayload().Get<FMeshPivotUpdateRequestInput>(), OutResult, OutError);
				}
				else
				{
					return MeshFailure(OutError, TEXT("world_spatial_object_mutation_request_mismatch"),
						TEXT("Object and mesh mutation received an unsupported request kind."), TEXT("/request_kind"),
						MakeShared<FJsonValueString>(Request.GetRequestKindId()),
						MeshChoices({TEXT("transform update"), TEXT("alignment"), TEXT("surface snap"), TEXT("mesh bake"), TEXT("mesh pivot")}), Contract.GetCapabilityId());
				}
				if (!bMutated)
				{
					return false;
				}
				OutResult = FUeAgentWorldSpatialMutationResult(
					TEXT("world-spatial:") + Context.GetRequestId(),
					TEXT("native-object-mesh-write:") + Contract.GetNativeActionId());
				return OutResult.TryValidate(OutError);
			}

			bool TrySettle(
				const FUeAgentWorldSpatialExecutionChainContract& Contract,
				const FUeAgentWorldSpatialMutationResult& Mutation,
				const FUeAgentOperationContext& Context,
				FUeAgentWorldSpatialExecutionError& OutError) override
			{
				return Mutation.TryValidate(OutError);
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
				if (Request.GetPayload().IsType<
					FWorldObjectTransformUpdateRequestInput>())
				{
					CompareUpdateTransform(
						Request.GetPayload().Get<
							FWorldObjectTransformUpdateRequestInput>(),
						After,
						Differences,
						OutError);
				}
				else if (Request.GetPayload().IsType<FWorldObjectAlignRequestInput>())
				{
					CompareAlign(
						Request.GetPayload().Get<FWorldObjectAlignRequestInput>(),
						Before,
						After,
						Differences,
						OutError);
				}
				else if (Request.GetPayload().IsType<FWorldObjectSnapToSurfaceRequestInput>())
				{
					CompareSnap(
						Request.GetPayload().Get<FWorldObjectSnapToSurfaceRequestInput>(),
						After,
						Differences,
						OutError);
				}
				else if (Request.GetPayload().IsType<FMeshTransformBakeRequestInput>())
				{
					CompareBake(
						Request.GetPayload().Get<FMeshTransformBakeRequestInput>(),
						Before,
						After,
						Differences,
						OutError);
				}
				else if (Request.GetPayload().IsType<FMeshPivotUpdateRequestInput>())
				{
					ComparePivot(
						Request.GetPayload().Get<FMeshPivotUpdateRequestInput>(),
						Before,
						After,
						Differences,
						OutError);
				}
				else
				{
					return MeshFailure(OutError, TEXT("world_spatial_object_compare_request_mismatch"),
						TEXT("Object and mesh verification requires a transform, alignment, snap, bake, or pivot request."), TEXT("/request_kind"),
						MakeShared<FJsonValueString>(Request.GetRequestKindId()),
						MeshChoices({TEXT("transform update"), TEXT("alignment"), TEXT("surface snap"), TEXT("mesh bake"), TEXT("mesh pivot")}), Contract.GetCapabilityId());
				}
				if (OutError.IsValid())
				{
					return false;
				}
				SetComparisonOutcome(
					Contract,
					MoveTemp(Differences),
					TEXT("object-mesh-independent-native-readback"),
					OutOutcome);
				return true;
			}

		private:
			bool TryReadMesh(
				const FUeAgentWorldSpatialExecutionChainContract& Contract,
				const FString& MeshRef,
				EUeAgentWorldSpatialReadPhase Phase,
				const FUeAgentOperationContext& Context,
				TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
				FUeAgentWorldSpatialExecutionError& OutError,
				const bool bProjectTargetRevision)
			{
				UStaticMesh* Mesh = nullptr;
				if (!TryResolveMesh(*Environment, MeshRef, Mesh, OutError))
				{
					return false;
				}
				const FMeshDescription* Description = Mesh->GetMeshDescription(0);
				if (!Description || Description->Vertices().Num() == 0)
				{
					return MeshFailure(OutError, TEXT("world_spatial_mesh_description_missing"),
						!Description ? TEXT("The static mesh has no LOD 0 mesh description.") : TEXT("The static mesh's LOD 0 description contains no vertices."),
						!Description ? TEXT("/lods/0/mesh_description") : TEXT("/lods/0/vertices"),
						Description ? MakeShared<FJsonValueNumber>(0) : TSharedPtr<FJsonValue>(),
						MakeShared<FJsonValueString>(TEXT("non-empty LOD 0 mesh description")), MeshRef);
				}
				FStaticMeshConstAttributes Attributes(*Description);
				const TVertexAttributesConstRef<FVector3f> Positions =
					Attributes.GetVertexPositions();
				TArray<FObservedEntry> Entries;
				FString Fingerprint = MeshRef;
				for (const FVertexID VertexId : Description->Vertices().GetElementIDs())
				{
					const FVector3f Position = Positions[VertexId];
					const FString Role = TEXT("mesh_vertex:")
						+ LexToString(VertexId.GetValue());
					FUeAgentCanonicalValuePtr Value;
					if (!TryMakeCompiledValue(
							MakePointValue(
								MeshRef,
								TEXT("mesh.geometry.local"),
								TEXT("centimeter"),
								FVector(Position),
								TEXT("mesh-description-vertex-readback")),
							Context,
							Value,
							OutError))
					{
						return false;
					}
					Entries.Add({
						TEXT("world-spatial:mesh-vertex:") + LexToString(VertexId.GetValue()),
						TEXT("mesh_vertex"),
						MeshRef,
						LexToString(VertexId.GetValue()),
						Role,
						TEXT("world_spatial.compiled_value.v1"),
						Value,
					});
					Fingerprint += TEXT("|") + Position.ToString();
				}

				TArray<UStaticMeshComponent*> Components;
				FindMeshComponents(Mesh, Components);
				for (UStaticMeshComponent* Component : Components)
				{
					const FString Role = TEXT("mesh_component:") + Component->GetPathName();
					FUeAgentCanonicalValuePtr Value;
					if (!TryMakeCompiledValue(
							MakeTransformValue(
								Component->GetPathName(),
								Component->GetComponentTransform(),
								TEXT("mesh-component-transform-readback")),
							Context,
							Value,
							OutError))
					{
						return false;
					}
					Entries.Add({
						TEXT("world-spatial:") + Role,
						TEXT("mesh_component"),
						MeshRef,
						Component->GetPathName(),
						Role,
						TEXT("world_spatial.compiled_value.v1"),
						Value,
					});
					Fingerprint += TEXT("|") + Component->GetComponentTransform().ToHumanReadableString();
				}
				return TryMakeSnapshot(
					Contract,
					Phase,
					Fingerprint,
					Entries,
					Context,
					OutSnapshot,
					OutError
				);
			}

			bool TryUpdateTransform(
				const FWorldObjectTransformUpdateRequestInput& Input,
				FUeAgentWorldSpatialMutationResult& OutResult,
				FUeAgentWorldSpatialExecutionError& OutError)
			{
				const FString& ActorRef = Input.Transform.Subject;
				if (Input.Transform.Quantity != TEXT("transform"))
				{
					return MeshFailure(OutError, TEXT("world_spatial_object_transform_quantity_invalid"),
						TEXT("Actor transform mutation requires a transform quantity."), TEXT("/transform/quantity"),
						MakeShared<FJsonValueString>(Input.Transform.Quantity), MakeShared<FJsonValueString>(TEXT("transform")), ActorRef);
				}
				FUeAgentWorldSpatialCompiledValue Compiled;
				if (!SpaceCompiler->TryConvertToNative(
						Input.Transform,
						TEXT("unreal.world"),
						TEXT("centimeter"),
						Compiled,
						OutError))
				{
					return false;
				}
				if (Compiled.Components.Num() != 10)
					return MeshFailure(OutError, TEXT("world_spatial_object_transform_components_invalid"),
						TEXT("Actor transforms require ten location, quaternion and scale components."), TEXT("/transform/components"),
						MakeShared<FJsonValueNumber>(Compiled.Components.Num()), MakeShared<FJsonValueNumber>(10), ActorRef);
				for (int32 Index = 0; Index < Compiled.Components.Num(); ++Index)
				{
					if (!FMath::IsFinite(Compiled.Components[Index]))
						return MeshFailure(OutError, TEXT("world_spatial_object_transform_components_invalid"),
							TEXT("An Actor transform component is not finite."), FString::Printf(TEXT("/transform/components/%d"), Index),
							MeshSpatialFact({Compiled.Components[Index]}, Compiled.FrameId, Index < 3 ? TEXT("centimeter") : TEXT("unitless")),
							MakeShared<FJsonValueString>(TEXT("finite component")), ActorRef);
				}
				const FQuat Rotation(
					Compiled.Components[3],
					Compiled.Components[4],
					Compiled.Components[5],
					Compiled.Components[6]);
				const FVector Scale(
					Compiled.Components[7],
					Compiled.Components[8],
					Compiled.Components[9]);
				if (!Rotation.IsNormalized())
					return MeshFailure(OutError, TEXT("world_spatial_object_transform_basis_invalid"),
						TEXT("The Actor transform rotation quaternion is not normalized."), TEXT("/transform/rotation"),
						MeshSpatialFact({Rotation.X, Rotation.Y, Rotation.Z, Rotation.W}, Compiled.FrameId, TEXT("unitless"), TEXT("quaternion_xyzw")),
						MakeShared<FJsonValueString>(TEXT("normalized quaternion")), ActorRef);
				for (int32 Axis = 0; Axis < 3; ++Axis)
				{
					if (FMath::IsNearlyZero(Scale[Axis]))
						return MeshFailure(OutError, TEXT("world_spatial_object_transform_basis_invalid"),
							TEXT("An Actor transform scale component is zero within the engine's near-zero tolerance."),
							FString::Printf(TEXT("/transform/components/%d"), Axis + 7),
							MeshSpatialFact({Scale[Axis]}, Compiled.FrameId, TEXT("unitless")),
							MakeShared<FJsonValueString>(TEXT("non-zero scale")), ActorRef);
				}
				const FTransform Desired(
					Rotation,
					FVector(
						Compiled.Components[0],
						Compiled.Components[1],
						Compiled.Components[2]),
					Scale);
				AActor* Actor = nullptr;
				if (!TryResolveActor(*Environment, ActorRef, Actor, OutError))
				{
					return false;
				}
				struct FScopedActorTransformRollback final
				{
					AActor* Actor = nullptr;
					FTransform Original;
					UPackage* Package = nullptr;
					bool bPackageDirty = false;
					bool bCommitted = false;

					~FScopedActorTransformRollback()
					{
						if (!Actor || bCommitted)
						{
							return;
						}
						Actor->SetActorTransform(
							Original,
							false,
							nullptr,
							ETeleportType::TeleportPhysics);
						Actor->PostEditMove(true);
						if (Package)
						{
							Package->SetDirtyFlag(bPackageDirty);
						}
					}
				};
				FScopedActorTransformRollback Rollback{
					Actor,
					Actor->GetActorTransform(),
					Actor->GetOutermost(),
					Actor->GetOutermost() && Actor->GetOutermost()->IsDirty(),
					false};
				OutResult = {}; // A native write may now occur; later failures are not NoMutation.
				Actor->Modify();
				if (!Actor->SetActorTransform(
						Desired,
						false,
						nullptr,
						ETeleportType::TeleportPhysics))
				{
					MeshFailure(OutError, TEXT("world_spatial_object_transform_native_write_failed"),
						TEXT("Unreal rejected the Actor transform update."), TEXT("/transform"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), ActorRef);
					const TSharedRef<FJsonObject> Proposed = MakeShared<FJsonObject>();
					Proposed->SetField(TEXT("location"), MeshSpatialFact({Compiled.Components[0], Compiled.Components[1], Compiled.Components[2]}, Compiled.FrameId, TEXT("centimeter")));
					Proposed->SetField(TEXT("rotation"), MeshSpatialFact({Rotation.X, Rotation.Y, Rotation.Z, Rotation.W}, Compiled.FrameId, TEXT("unitless"), TEXT("quaternion_xyzw")));
					Proposed->SetField(TEXT("scale"), MeshSpatialFact({Scale.X, Scale.Y, Scale.Z}, Compiled.FrameId, TEXT("unitless")));
					OutError.ProposedValue = MakeShared<FJsonValueObject>(Proposed);
					return false;
				}
				Actor->PostEditMove(true);
				if (!Environment->TryPersistObject(*Actor, OutError))
				{
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = ActorRef;
					return false;
				}
				Rollback.bCommitted = true;
				return true;
			}

			bool TryAlign(
				const FWorldObjectAlignRequestInput& Input,
				FUeAgentWorldSpatialMutationResult& OutResult,
				FUeAgentWorldSpatialExecutionError& OutError)
			{
				AActor* ReferenceActor = nullptr;
				if (Input.Alignment.Kind != TEXT("anchors"))
				{
					if (!Input.AnchorRef.IsSet())
						return MeshFailure(OutError, TEXT("world_spatial_alignment_reference_missing"),
							TEXT("This alignment kind requires an anchor Actor reference."), TEXT("/anchor_ref"),
							nullptr, MakeShared<FJsonValueString>(TEXT("explicit anchor Actor reference")));
					if (!TryResolveActor(*Environment, Input.AnchorRef.GetValue(), ReferenceActor, OutError)) return false;
				}

				FUeAgentWorldSpatialCompiledValue ExplicitSubjectAnchor;
				FUeAgentWorldSpatialCompiledValue ExplicitReferenceAnchor;
				FQuat ExplicitRotationDelta = FQuat::Identity;
				if (Input.Alignment.Kind == TEXT("anchors"))
				{
					if (!Input.Alignment.SubjectAnchor.IsSet() || !Input.Alignment.ReferenceAnchor.IsSet())
						return MeshFailure(OutError, TEXT("world_spatial_alignment_anchor_missing"),
							!Input.Alignment.SubjectAnchor.IsSet() ? TEXT("Anchor alignment requires a subject anchor.") : TEXT("Anchor alignment requires a reference anchor."),
							!Input.Alignment.SubjectAnchor.IsSet() ? TEXT("/alignment/subject_anchor") : TEXT("/alignment/reference_anchor"),
							nullptr, MakeShared<FJsonValueString>(TEXT("explicit anchor point")));
					if (!SpaceCompiler->TryConvertToNative(
							Input.Alignment.SubjectAnchor.GetValue(),
							TEXT("unreal.world"), TEXT("centimeter"),
							ExplicitSubjectAnchor, OutError)
						|| !SpaceCompiler->TryConvertToNative(
							Input.Alignment.ReferenceAnchor.GetValue(),
							TEXT("unreal.world"), TEXT("centimeter"),
							ExplicitReferenceAnchor, OutError))
					{
						return false;
					}
					if (Input.Alignment.RotationPolicy != TEXT("preserve"))
					{
						if (!Input.Alignment.SubjectNormal.IsSet()
							|| !Input.Alignment.ReferenceNormal.IsSet())
						{
							return MeshFailure(OutError, TEXT("world_spatial_alignment_normals_missing"),
								!Input.Alignment.SubjectNormal.IsSet() ? TEXT("Rotating anchor alignment requires a subject normal.") : TEXT("Rotating anchor alignment requires a reference normal."),
								!Input.Alignment.SubjectNormal.IsSet() ? TEXT("/alignment/subject_normal") : TEXT("/alignment/reference_normal"),
								nullptr, MakeShared<FJsonValueString>(TEXT("explicit normal for the rotation policy")));
						}
						FUeAgentWorldSpatialCompiledValue SubjectNormal;
						FUeAgentWorldSpatialCompiledValue ReferenceNormal;
						if (!SpaceCompiler->TryConvertToNative(
								Input.Alignment.SubjectNormal.GetValue(),
								TEXT("unreal.world"), TEXT("unitless"), SubjectNormal, OutError)
							|| !SpaceCompiler->TryConvertToNative(
								Input.Alignment.ReferenceNormal.GetValue(),
								TEXT("unreal.world"), TEXT("unitless"), ReferenceNormal, OutError))
						{
							return false;
						}
						ExplicitRotationDelta = FQuat::FindBetweenNormals(
							FVector(SubjectNormal.Components[0], SubjectNormal.Components[1], SubjectNormal.Components[2]).GetSafeNormal(),
							FVector(ReferenceNormal.Components[0], ReferenceNormal.Components[1], ReferenceNormal.Components[2]).GetSafeNormal());
					}
				}

				TArray<TPair<AActor*, FTransform>> PlannedTransforms;
				for (const FString& Ref : Input.SubjectRefs)
				{
					AActor* Actor = nullptr;
					if (!TryResolveActor(*Environment, Ref, Actor, OutError))
					{
						return false;
					}
					FVector Delta = FVector::ZeroVector;
					if (Input.Alignment.Kind == TEXT("anchors"))
					{
						Delta = FVector(
							ExplicitReferenceAnchor.Components[0] - ExplicitSubjectAnchor.Components[0],
							ExplicitReferenceAnchor.Components[1] - ExplicitSubjectAnchor.Components[1],
							ExplicitReferenceAnchor.Components[2] - ExplicitSubjectAnchor.Components[2]);
					}
					else if (Input.Alignment.Kind == TEXT("bounds")
						|| Input.Alignment.Kind == TEXT("face"))
					{
						int32 SubjectAxis = INDEX_NONE;
						int32 ReferenceAxis = INDEX_NONE;
						FString SubjectAnchor = Input.Alignment.SubjectBoundsAnchor;
						FString ReferenceAnchor = Input.Alignment.ReferenceBoundsAnchor;
						if (Input.Alignment.Kind == TEXT("bounds"))
						{
							if (!TryAxisIndex(Input.Alignment.Axis, SubjectAxis, OutError, Ref)) return false;
							ReferenceAxis = SubjectAxis;
						}
						else if (!TryResolveFace(Input.Alignment.SubjectFace, Input.Alignment.Axis, SubjectAxis, SubjectAnchor, OutError, TEXT("/alignment/subject_face"), Ref)
							|| !TryResolveFace(Input.Alignment.ReferenceFace, Input.Alignment.Axis, ReferenceAxis, ReferenceAnchor, OutError, TEXT("/alignment/reference_face"), Input.AnchorRef.GetValue()))
						{
							return false;
						}
						if (SubjectAxis != ReferenceAxis)
						{
							static const TCHAR* AxisNames[] = {TEXT("x"), TEXT("y"), TEXT("z")};
							return MeshFailure(OutError, TEXT("world_spatial_alignment_face_axis_mismatch"),
								TEXT("The subject and reference alignment faces must use the same axis."), TEXT("/alignment/subject_face"),
								MakeShared<FJsonValueString>(AxisNames[SubjectAxis]), MakeShared<FJsonValueString>(AxisNames[ReferenceAxis]), Ref);
						}
						double SubjectValue = 0.0;
						double ReferenceValue = 0.0;
						if (!TryBoundsAnchorValue(Actor->GetComponentsBoundingBox(true), SubjectAxis, SubjectAnchor, SubjectValue, OutError, TEXT("/alignment/subject_bounds_anchor"), Ref)
							|| !TryBoundsAnchorValue(ReferenceActor->GetComponentsBoundingBox(true), ReferenceAxis, ReferenceAnchor, ReferenceValue, OutError, TEXT("/alignment/reference_bounds_anchor"), Input.AnchorRef.GetValue()))
						{
							return false;
						}
						Delta = AxisDelta(SubjectAxis, ReferenceValue - SubjectValue + Input.Alignment.OffsetCm);
					}
					else if (Input.Alignment.Kind == TEXT("vertex"))
					{
						TArray<FVector> SubjectVertices;
						TArray<FVector> ReferenceVertices;
						if (!TryActorWorldVertices(*Actor, SubjectVertices, OutError, Ref)
							|| !TryActorWorldVertices(*ReferenceActor, ReferenceVertices, OutError, Input.AnchorRef.GetValue()))
						{
							return false;
						}
						int32 SubjectIndex = INDEX_NONE;
						int32 ReferenceIndex = INDEX_NONE;
						if (Input.Alignment.SubjectVertexIndex.IsSet()
							|| Input.Alignment.ReferenceVertexIndex.IsSet())
						{
							if (!Input.Alignment.SubjectVertexIndex.IsSet()
								|| !Input.Alignment.ReferenceVertexIndex.IsSet())
							{
								return MeshFailure(OutError, TEXT("world_spatial_alignment_vertex_pair_incomplete"),
									TEXT("Explicit vertex alignment requires both subject and reference vertex indices."),
									!Input.Alignment.SubjectVertexIndex.IsSet() ? TEXT("/alignment/subject_vertex_index") : TEXT("/alignment/reference_vertex_index"),
									nullptr, MakeShared<FJsonValueString>(TEXT("paired subject and reference vertex indices")), Ref);
							}
							SubjectIndex = Input.Alignment.SubjectVertexIndex.GetValue();
							ReferenceIndex = Input.Alignment.ReferenceVertexIndex.GetValue();
							if (!SubjectVertices.IsValidIndex(SubjectIndex)
								|| !ReferenceVertices.IsValidIndex(ReferenceIndex))
							{
								const bool bSubjectInvalid = !SubjectVertices.IsValidIndex(SubjectIndex);
								const TSharedRef<FJsonObject> Range = MakeShared<FJsonObject>();
								Range->SetNumberField(TEXT("minimum"), 0);
								Range->SetNumberField(TEXT("maximum_exclusive"), bSubjectInvalid ? SubjectVertices.Num() : ReferenceVertices.Num());
								return MeshFailure(OutError, TEXT("world_spatial_alignment_vertex_index_invalid"),
									bSubjectInvalid ? TEXT("The subject vertex index is outside the available vertex array.") : TEXT("The reference vertex index is outside the available vertex array."),
									bSubjectInvalid ? TEXT("/alignment/subject_vertex_index") : TEXT("/alignment/reference_vertex_index"),
									MakeShared<FJsonValueNumber>(bSubjectInvalid ? SubjectIndex : ReferenceIndex), MakeShared<FJsonValueObject>(Range),
									bSubjectInvalid ? Ref : Input.AnchorRef.GetValue());
							}
						}
						else
						{
							double BestDistanceSquared = TNumericLimits<double>::Max();
							for (int32 SourceIndex = 0; SourceIndex < SubjectVertices.Num(); ++SourceIndex)
							{
								for (int32 TargetIndex = 0; TargetIndex < ReferenceVertices.Num(); ++TargetIndex)
								{
									const double DistanceSquared = FVector::DistSquared(SubjectVertices[SourceIndex], ReferenceVertices[TargetIndex]);
									if (DistanceSquared < BestDistanceSquared)
									{
										BestDistanceSquared = DistanceSquared;
										SubjectIndex = SourceIndex;
										ReferenceIndex = TargetIndex;
									}
								}
							}
						}
						Delta = ReferenceVertices[ReferenceIndex] - SubjectVertices[SubjectIndex];
					}
					else
					{
						return MeshFailure(OutError, TEXT("world_spatial_alignment_kind_invalid"),
							TEXT("The alignment kind is not supported."), TEXT("/alignment/kind"), MakeShared<FJsonValueString>(Input.Alignment.Kind),
							MeshChoices({TEXT("anchors"), TEXT("bounds"), TEXT("face"), TEXT("vertex")}), Ref);
					}
					FTransform Planned = Actor->GetActorTransform();
					Planned.SetLocation(Planned.GetLocation() + Delta);
					if (Input.Alignment.Kind == TEXT("anchors")
						&& Input.Alignment.RotationPolicy != TEXT("preserve"))
					{
						Planned.SetRotation(ExplicitRotationDelta * Planned.GetRotation());
					}
					PlannedTransforms.Add({Actor, Planned});
				}

				for (const TPair<AActor*, FTransform>& Planned : PlannedTransforms)
				{
					OutResult = {};
					Planned.Key->Modify();
					Planned.Key->SetActorTransform(
						Planned.Value, false, nullptr, ETeleportType::TeleportPhysics);
				}
				return true;
			}

			bool TrySnap(
				const FWorldObjectSnapToSurfaceRequestInput& Input,
				FUeAgentWorldSpatialMutationResult& OutResult,
				FUeAgentWorldSpatialExecutionError& OutError)
			{
				FHitResult Hit;
				if (!TryTraceSurface(*Environment, *SpaceCompiler, Input, Hit, OutError))
				{
					return false;
				}
				for (const FString& Ref : Input.SubjectRefs)
				{
					AActor* Actor = nullptr;
					if (!TryResolveActor(*Environment, Ref, Actor, OutError))
					{
						return false;
					}
					OutResult = {};
					Actor->SetActorLocation(
						Hit.ImpactPoint
							+ Hit.ImpactNormal * Input.PlacementPolicy.ClearanceCm,
						false,
						nullptr,
						ETeleportType::TeleportPhysics);
					if (Input.PlacementPolicy.bAlignToSurfaceNormal)
					{
						const FRotator Original = Actor->GetActorRotation();
						FRotator Aligned = FRotationMatrix::MakeFromZ(
							Hit.ImpactNormal.GetSafeNormal()).Rotator();
						if (Input.PlacementPolicy.bPreserveYaw)
						{
							Aligned.Yaw = Original.Yaw;
						}
						Actor->SetActorRotation(Aligned, ETeleportType::TeleportPhysics);
					}
				}
				return true;
			}

			bool TryPersistMesh(
				UStaticMesh& Mesh,
				const FString& MeshRef,
				FUeAgentWorldSpatialExecutionError& OutError)
			{
				if (Environment->TryPersistObject(Mesh, OutError))
				{
					return true;
				}
				if (!OutError.IsValid())
				{
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("world_spatial_mesh_persistence_failed"),
						TEXT("The native persistence provider failed without returning a diagnostic."), TEXT("native_execution"));
				}
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = MeshRef;
				return false;
			}

			bool TryBake(
				const FMeshTransformBakeRequestInput& Input,
				FUeAgentWorldSpatialMutationResult& OutResult,
				FUeAgentWorldSpatialExecutionError& OutError)
			{
				const FString& MeshRef = Input.Transform.Subject;
				FUeAgentWorldSpatialCompiledValue Compiled;
				if (!SpaceCompiler->TryConvertToNative(
						Input.Transform,
						TEXT("mesh.asset.local"),
						TEXT("centimeter"),
						Compiled,
						OutError))
				{
					return false;
				}
				UStaticMesh* Mesh = nullptr;
				if (!TryResolveMesh(*Environment, MeshRef, Mesh, OutError))
				{
					return false;
				}
				FMeshDescription* Description = Mesh->GetMeshDescription(0);
				if (!Description)
				{
					return MeshFailure(OutError, TEXT("world_spatial_mesh_description_missing"),
						TEXT("The static mesh has no LOD 0 mesh description to bake."), TEXT("/lods/0/mesh_description"),
						nullptr, MakeShared<FJsonValueString>(TEXT("LOD 0 mesh description")), MeshRef);
				}
				const FTransform Transform(
					FQuat(
						Compiled.Components[3], Compiled.Components[4],
						Compiled.Components[5], Compiled.Components[6]).GetNormalized(),
					FVector(
						Compiled.Components[0], Compiled.Components[1], Compiled.Components[2]),
					FVector(
						Compiled.Components[7], Compiled.Components[8], Compiled.Components[9]));
				FStaticMeshAttributes Attributes(*Description);
				TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
				OutResult = {};
				Mesh->Modify();
				for (const FVertexID VertexId : Description->Vertices().GetElementIDs())
				{
					Positions[VertexId] = FVector3f(
						Transform.TransformPosition(FVector(Positions[VertexId])));
				}
				Mesh->CommitMeshDescription(0);
				Mesh->PostEditChange();
				if (Mesh->GetMeshDescription(0) == nullptr)
				{
					return MeshFailure(OutError, TEXT("world_spatial_mesh_post_edit_invalid"),
						TEXT("The mesh no longer has a LOD 0 description after the transform bake was committed."), TEXT("/lods/0/mesh_description"),
						nullptr, MakeShared<FJsonValueString>(TEXT("LOD 0 mesh description after edit")), MeshRef);
				}
				return TryPersistMesh(*Mesh, MeshRef, OutError);
			}

			bool TryPivot(
				const FMeshPivotUpdateRequestInput& Input,
				FUeAgentWorldSpatialMutationResult& OutResult,
				FUeAgentWorldSpatialExecutionError& OutError)
			{
				const FString& MeshRef = Input.Pivot.Subject;
				FUeAgentWorldSpatialCompiledValue Compiled;
				if (!SpaceCompiler->TryConvertToNative(
						Input.Pivot,
						TEXT("mesh.geometry.local"),
						TEXT("centimeter"),
						Compiled,
						OutError))
				{
					return false;
				}
				const FVector Pivot(
					Compiled.Components[0], Compiled.Components[1], Compiled.Components[2]);
				UStaticMesh* Mesh = nullptr;
				if (!TryResolveMesh(*Environment, MeshRef, Mesh, OutError))
				{
					return false;
				}
				FMeshDescription* Description = Mesh->GetMeshDescription(0);
				if (!Description)
				{
					return MeshFailure(OutError, TEXT("world_spatial_mesh_description_missing"),
						TEXT("The static mesh has no LOD 0 mesh description for a pivot update."), TEXT("/lods/0/mesh_description"),
						nullptr, MakeShared<FJsonValueString>(TEXT("LOD 0 mesh description")), MeshRef);
				}
				TArray<UStaticMeshComponent*> Components;
				if (Input.PreserveWorldGeometry.Get(false))
				{
					FindMeshComponents(Mesh, Components);
				}
				FStaticMeshAttributes Attributes(*Description);
				TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
				OutResult = {};
				Mesh->Modify();
				for (const FVertexID VertexId : Description->Vertices().GetElementIDs())
				{
					Positions[VertexId] -= FVector3f(Pivot);
				}
				Mesh->CommitMeshDescription(0);
				Mesh->PostEditChange();
				if (Mesh->GetMeshDescription(0) == nullptr)
				{
					return MeshFailure(OutError, TEXT("world_spatial_mesh_post_edit_invalid"),
						TEXT("The mesh no longer has a LOD 0 description after the pivot edit was committed."), TEXT("/lods/0/mesh_description"),
						nullptr, MakeShared<FJsonValueString>(TEXT("LOD 0 mesh description after edit")), MeshRef);
				}
				if (!TryPersistMesh(*Mesh, MeshRef, OutError))
				{
					return false;
				}
				for (UStaticMeshComponent* Component : Components)
				{
					Component->Modify();
					const FTransform Transform = Component->GetComponentTransform();
					Component->SetWorldLocation(
						Transform.GetLocation() + Transform.TransformVector(Pivot));
				}
				return true;
			}

			bool CompareUpdateTransform(
				const FWorldObjectTransformUpdateRequestInput& Input,
				const FUeAgentCanonicalSnapshot& After,
				TArray<UE::UAI::Contract::FDiagnostic>& Differences,
				FUeAgentWorldSpatialExecutionError& OutError)
			{
				FUeAgentWorldSpatialCompiledValue Expected;
				FUeAgentWorldSpatialCompiledValue Actual;
				if (!SpaceCompiler->TryConvertToNative(
						Input.Transform,
						TEXT("unreal.world"),
						TEXT("centimeter"),
						Expected,
						OutError)
					|| !TryReadCompiledValue(
						After,
						TEXT("actor_transform:") + Input.Transform.Subject,
						Actual,
						OutError))
				{
					return false;
				}
				return CompareTransforms(Expected, Actual, Differences, TEXT("object.transform"), Input.Transform.Subject, 0.001);
			}

			bool CompareAlign(
				const FWorldObjectAlignRequestInput& Input,
				const FUeAgentCanonicalSnapshot& Before,
				const FUeAgentCanonicalSnapshot& After,
				TArray<UE::UAI::Contract::FDiagnostic>& Differences,
				FUeAgentWorldSpatialExecutionError& OutError)
			{
				if (Input.Alignment.Kind == TEXT("anchors"))
				{
					if (!Input.Alignment.SubjectAnchor.IsSet()
						|| !Input.Alignment.ReferenceAnchor.IsSet())
					{
						AddComparisonDifference(Differences, !Input.Alignment.SubjectAnchor.IsSet()
							? TEXT("alignment.subject_anchor") : TEXT("alignment.reference_anchor"),
							TEXT("Anchor alignment verification requires both spatial anchors."), MakeShared<FJsonValueString>(TEXT("spatial anchor")), nullptr);
						return false;
					}
					FUeAgentWorldSpatialCompiledValue SubjectAnchor;
					FUeAgentWorldSpatialCompiledValue ReferenceAnchor;
					if (!SpaceCompiler->TryConvertToNative(
							Input.Alignment.SubjectAnchor.GetValue(),
							TEXT("unreal.world"), TEXT("centimeter"), SubjectAnchor, OutError)
						|| !SpaceCompiler->TryConvertToNative(
							Input.Alignment.ReferenceAnchor.GetValue(),
							TEXT("unreal.world"), TEXT("centimeter"), ReferenceAnchor, OutError))
					{
						return false;
					}
					const FVector Delta(
						ReferenceAnchor.Components[0] - SubjectAnchor.Components[0],
						ReferenceAnchor.Components[1] - SubjectAnchor.Components[1],
						ReferenceAnchor.Components[2] - SubjectAnchor.Components[2]);
					for (const FString& Ref : Input.SubjectRefs)
					{
						FUeAgentWorldSpatialCompiledValue OldValue;
						FUeAgentWorldSpatialCompiledValue NewValue;
						if (!TryReadCompiledValue(Before, TEXT("actor_transform:") + Ref, OldValue, OutError)
							|| !TryReadCompiledValue(After, TEXT("actor_transform:") + Ref, NewValue, OutError))
						{
							return false;
						}
						const FVector Expected(
							OldValue.Components[0] + Delta.X,
							OldValue.Components[1] + Delta.Y,
							OldValue.Components[2] + Delta.Z);
						if (!Expected.Equals(
								FVector(NewValue.Components[0], NewValue.Components[1], NewValue.Components[2]),
								0.001))
						{
							AddComparisonDifference(Differences, TEXT("alignment.location"), TEXT("The subject location differs from the expected anchor displacement."),
								ComparisonComponents({Expected.X, Expected.Y, Expected.Z}),
								ComparisonComponents({NewValue.Components[0], NewValue.Components[1], NewValue.Components[2]}), Ref);
							return false;
						}
					}
					return true;
				}

				if (!Input.AnchorRef.IsSet())
				{
					AddComparisonDifference(Differences, TEXT("alignment.anchor_ref"), TEXT("Alignment verification requires a reference actor."),
						MakeShared<FJsonValueString>(TEXT("reference actor")), nullptr);
					return false;
				}
				AActor* ReferenceActor = nullptr;
				if (!TryResolveActor(*Environment, Input.AnchorRef.GetValue(), ReferenceActor, OutError))
				{
					return false;
				}
				for (const FString& Ref : Input.SubjectRefs)
				{
					AActor* Actor = nullptr;
					if (!TryResolveActor(*Environment, Ref, Actor, OutError))
					{
						return false;
					}
					if (Input.Alignment.Kind == TEXT("bounds")
						|| Input.Alignment.Kind == TEXT("face"))
					{
						int32 SubjectAxis = INDEX_NONE;
						int32 ReferenceAxis = INDEX_NONE;
						FString SubjectAnchor = Input.Alignment.SubjectBoundsAnchor;
						FString ReferenceAnchor = Input.Alignment.ReferenceBoundsAnchor;
						if (Input.Alignment.Kind == TEXT("bounds"))
						{
							if (!TryAxisIndex(Input.Alignment.Axis, SubjectAxis, OutError, Ref)) return false;
							ReferenceAxis = SubjectAxis;
						}
						else if (!TryResolveFace(Input.Alignment.SubjectFace, Input.Alignment.Axis, SubjectAxis, SubjectAnchor, OutError, TEXT("/alignment/subject_face"), Ref)
							|| !TryResolveFace(Input.Alignment.ReferenceFace, Input.Alignment.Axis, ReferenceAxis, ReferenceAnchor, OutError, TEXT("/alignment/reference_face"), Input.AnchorRef.GetValue()))
						{
							return false;
						}
						double SubjectValue = 0.0;
						double ReferenceValue = 0.0;
						if (SubjectAxis != ReferenceAxis)
						{
							AddComparisonDifference(Differences, TEXT("alignment.axis"), TEXT("The subject and reference bounds use different axes."),
								ComparisonNumber(ReferenceAxis), ComparisonNumber(SubjectAxis), Ref);
							return false;
						}
						if (!TryBoundsAnchorValue(Actor->GetComponentsBoundingBox(true), SubjectAxis, SubjectAnchor, SubjectValue, OutError, TEXT("/alignment/subject_bounds_anchor"), Ref)
							|| !TryBoundsAnchorValue(ReferenceActor->GetComponentsBoundingBox(true), ReferenceAxis, ReferenceAnchor, ReferenceValue, OutError, TEXT("/alignment/reference_bounds_anchor"), Input.AnchorRef.GetValue()))
						{
							return false;
						}
						if (!FMath::IsNearlyEqual(SubjectValue, ReferenceValue + Input.Alignment.OffsetCm, 0.01))
						{
							AddComparisonDifference(Differences, TEXT("alignment.bounds_anchor"), TEXT("The subject bounds anchor does not match the reference anchor and requested offset in centimeters."),
								ComparisonNumber(ReferenceValue + Input.Alignment.OffsetCm), ComparisonNumber(SubjectValue), Ref);
							return false;
						}
					}
					else if (Input.Alignment.Kind == TEXT("vertex"))
					{
						TArray<FVector> SubjectVertices;
						TArray<FVector> ReferenceVertices;
						if (!TryActorWorldVertices(*Actor, SubjectVertices, OutError, Ref)
							|| !TryActorWorldVertices(*ReferenceActor, ReferenceVertices, OutError, Input.AnchorRef.GetValue()))
						{
							return false;
						}
						double MinimumDistanceSquared = TNumericLimits<double>::Max();
						for (const FVector& Source : SubjectVertices)
						{
							for (const FVector& TargetVertex : ReferenceVertices)
							{
								MinimumDistanceSquared = FMath::Min(
									MinimumDistanceSquared,
									FVector::DistSquared(Source, TargetVertex));
							}
						}
						if (MinimumDistanceSquared > FMath::Square(0.01))
						{
							AddComparisonDifference(Differences, TEXT("alignment.minimum_vertex_distance_squared"),
								TEXT("The minimum squared vertex distance exceeds the alignment tolerance in squared centimeters."),
								ComparisonNumber(FMath::Square(0.01)), ComparisonNumber(MinimumDistanceSquared), Ref);
							return false;
						}
					}
					else
					{
						AddComparisonDifference(Differences, TEXT("alignment.kind"), TEXT("The native verifier does not support this alignment kind."),
							MakeShared<FJsonValueString>(TEXT("anchors, bounds, face, or vertex")), MakeShared<FJsonValueString>(Input.Alignment.Kind), Ref);
						return false;
					}
				}
				return true;
			}

			bool CompareSnap(
				const FWorldObjectSnapToSurfaceRequestInput& Input,
				const FUeAgentCanonicalSnapshot& After,
				TArray<UE::UAI::Contract::FDiagnostic>& Differences,
				FUeAgentWorldSpatialExecutionError& OutError)
			{
				FUeAgentWorldSpatialCompiledValue Hit;
				FUeAgentWorldSpatialCompiledValue Normal;
				if (!TryReadCompiledValue(
						After, TEXT("surface_hit.point"), Hit, OutError)
					|| !TryReadCompiledValue(
						After, TEXT("surface_hit.normal"), Normal, OutError))
				{
					return false;
				}
				const FVector Expected(
					Hit.Components[0] + Normal.Components[0] * Input.PlacementPolicy.ClearanceCm,
					Hit.Components[1] + Normal.Components[1] * Input.PlacementPolicy.ClearanceCm,
					Hit.Components[2] + Normal.Components[2] * Input.PlacementPolicy.ClearanceCm);
				for (const FString& Ref : Input.SubjectRefs)
				{
					FUeAgentWorldSpatialCompiledValue Actual;
					if (!TryReadCompiledValue(
							After, TEXT("actor_transform:") + Ref, Actual, OutError))
					{
						return false;
					}
					if (!Expected.Equals(
							FVector(
								Actual.Components[0],
								Actual.Components[1],
								Actual.Components[2]),
							0.001))
					{
						AddComparisonDifference(Differences, TEXT("snap.location"), TEXT("The subject location differs from the hit point plus the requested normal clearance."),
							ComparisonComponents({Expected.X, Expected.Y, Expected.Z}),
							ComparisonComponents({Actual.Components[0], Actual.Components[1], Actual.Components[2]}), Ref);
						return false;
					}
				}
				return true;
			}

			bool CompareBake(
				const FMeshTransformBakeRequestInput& Input,
				const FUeAgentCanonicalSnapshot& Before,
				const FUeAgentCanonicalSnapshot& After,
				TArray<UE::UAI::Contract::FDiagnostic>& Differences,
				FUeAgentWorldSpatialExecutionError& OutError)
			{
				FUeAgentWorldSpatialCompiledValue Compiled;
				if (!SpaceCompiler->TryConvertToNative(
						Input.Transform,
						TEXT("mesh.asset.local"),
						TEXT("centimeter"),
						Compiled,
						OutError))
				{
					return false;
				}
				const FTransform Transform(
					FQuat(
						Compiled.Components[3], Compiled.Components[4],
						Compiled.Components[5], Compiled.Components[6]).GetNormalized(),
					FVector(
						Compiled.Components[0], Compiled.Components[1], Compiled.Components[2]),
					FVector(
						Compiled.Components[7], Compiled.Components[8], Compiled.Components[9]));
				for (const FUeAgentStateEntry& Entry : Before.GetEntries())
				{
					const FString& Role = Entry.GetIdentity().GetObjectRole();
					if (!Role.StartsWith(TEXT("mesh_vertex:")))
					{
						continue;
					}
					FUeAgentWorldSpatialCompiledValue OldValue;
					FUeAgentWorldSpatialCompiledValue NewValue;
					if (!TryReadCompiledValue(Before, Role, OldValue, OutError)
						|| !TryReadCompiledValue(After, Role, NewValue, OutError))
					{
						return false;
					}
					const FVector Expected = Transform.TransformPosition(FVector(
						OldValue.Components[0], OldValue.Components[1], OldValue.Components[2]));
					if (!Expected.Equals(
							FVector(
								NewValue.Components[0],
								NewValue.Components[1],
								NewValue.Components[2]),
							0.001))
					{
						AddComparisonDifference(Differences, Role + TEXT(".position"), TEXT("The baked mesh vertex differs from the expected transformed position."),
							ComparisonComponents({Expected.X, Expected.Y, Expected.Z}),
							ComparisonComponents({NewValue.Components[0], NewValue.Components[1], NewValue.Components[2]}), Input.Transform.Subject);
						return false;
					}
				}
				return true;
			}

			bool ComparePivot(
				const FMeshPivotUpdateRequestInput& Input,
				const FUeAgentCanonicalSnapshot& Before,
				const FUeAgentCanonicalSnapshot& After,
				TArray<UE::UAI::Contract::FDiagnostic>& Differences,
				FUeAgentWorldSpatialExecutionError& OutError)
			{
				FUeAgentWorldSpatialCompiledValue Compiled;
				if (!SpaceCompiler->TryConvertToNative(
						Input.Pivot,
						TEXT("mesh.geometry.local"),
						TEXT("centimeter"),
						Compiled,
						OutError))
				{
					return false;
				}
				const FVector Pivot(
					Compiled.Components[0], Compiled.Components[1], Compiled.Components[2]);
				for (const FUeAgentStateEntry& Entry : Before.GetEntries())
				{
					const FString& Role = Entry.GetIdentity().GetObjectRole();
					if (!Role.StartsWith(TEXT("mesh_vertex:")))
					{
						continue;
					}
					FUeAgentWorldSpatialCompiledValue OldValue;
					FUeAgentWorldSpatialCompiledValue NewValue;
					if (!TryReadCompiledValue(Before, Role, OldValue, OutError)
						|| !TryReadCompiledValue(After, Role, NewValue, OutError))
					{
						return false;
					}
					const FVector Expected(
						OldValue.Components[0] - Pivot.X,
						OldValue.Components[1] - Pivot.Y,
						OldValue.Components[2] - Pivot.Z);
					if (!Expected.Equals(
							FVector(
								NewValue.Components[0],
								NewValue.Components[1],
								NewValue.Components[2]),
							0.001))
					{
						AddComparisonDifference(Differences, Role + TEXT(".position"), TEXT("The mesh vertex differs from the expected pivot displacement."),
							ComparisonComponents({Expected.X, Expected.Y, Expected.Z}),
							ComparisonComponents({NewValue.Components[0], NewValue.Components[1], NewValue.Components[2]}), Input.Pivot.Subject);
						return false;
					}
				}
				return true;
			}

			TSharedRef<IUeAgentWorldSpatialNativeEnvironment> Environment;
			TSharedRef<IUeAgentWorldSpatialSpaceCompiler> SpaceCompiler;
			TSharedRef<IUeAgentWorldSpatialRevisionSource> RevisionSource;
		};
	}

	FWorldSpatialNativeFamilyRef MakeObjectTransformNativeFamily(
		const FUeAgentWorldSpatialNativeDependencies& Dependencies)
	{
		return MakeShared<FObjectTransformNativeFamily>(Dependencies);
	}
}

