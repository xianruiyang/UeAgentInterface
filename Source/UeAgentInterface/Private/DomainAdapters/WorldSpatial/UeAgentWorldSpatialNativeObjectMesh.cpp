#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialNativeInternal.h"


#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "UObject/UObjectIterator.h"

namespace UE::UAI::DomainAdapters::WorldSpatial::Private
{
	using namespace StateSemantic;

	namespace
	{
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
			for (const FString& Lexeme : Value.Components)
			{
				double Parsed = 0.0;
				if (!LexTryParseString(Parsed, *Lexeme))
				{
					return Fail(
						OutError,
						TEXT("world_spatial_compiled_number_invalid"),
						Value.Subject);
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
			return OutValue.IsValid();
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
				: Fail(
					OutError,
					TEXT("world_spatial_actor_type_mismatch"),
					Ref);
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
				: Fail(
					OutError,
					TEXT("world_spatial_mesh_type_mismatch"),
					Ref);
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
				return Fail(
					OutError,
					TEXT("world_spatial_surface_direction_zero"),
					Input.SurfaceQuery.Direction.Subject);
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
			if (!World || !bChannelValid)
			{
				return Fail(
					OutError,
					TEXT("world_spatial_surface_trace_context_invalid"),
					Input.SurfaceQuery.TraceChannel);
			}
			const FVector End = Start
				+ DirectionVector.GetSafeNormal()
					* Input.SurfaceQuery.MaxDistanceCm;
			return World->LineTraceSingleByChannel(
					OutHit, Start, End, Channel, Params)
				? true
				: Fail(
					OutError,
					TEXT("world_spatial_surface_hit_missing"),
					Input.SurfaceQuery.Origin.Subject);
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
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			if (Axis == TEXT("x")) OutIndex = 0;
			else if (Axis == TEXT("y")) OutIndex = 1;
			else if (Axis == TEXT("z")) OutIndex = 2;
			else return Fail(OutError, TEXT("world_spatial_alignment_axis_invalid"), Axis);
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
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			if (!Bounds.IsValid)
			{
				return Fail(OutError, TEXT("world_spatial_alignment_bounds_invalid"), Anchor);
			}
			if (Anchor == TEXT("min")) OutValue = AxisValue(Bounds.Min, AxisIndex);
			else if (Anchor == TEXT("max")) OutValue = AxisValue(Bounds.Max, AxisIndex);
			else if (Anchor == TEXT("center")) OutValue = AxisValue(Bounds.GetCenter(), AxisIndex);
			else return Fail(OutError, TEXT("world_spatial_alignment_anchor_invalid"), Anchor);
			return true;
		}

		bool TryResolveFace(
			const FString& Face,
			const FString& FallbackAxis,
			int32& OutAxisIndex,
			FString& OutAnchor,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			const FString Normalized = Face.ToLower();
			if (Normalized == TEXT("min") || Normalized == TEXT("center") || Normalized == TEXT("max"))
			{
				OutAnchor = Normalized;
				return TryAxisIndex(FallbackAxis, OutAxisIndex, OutError);
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
			return Fail(OutError, TEXT("world_spatial_alignment_face_invalid"), Face);
		}

		bool TryActorWorldVertices(
			AActor& Actor,
			TArray<FVector>& OutVertices,
			FUeAgentWorldSpatialExecutionError& OutError)
		{
			OutVertices.Reset();
			UStaticMeshComponent* Component = Actor.FindComponentByClass<UStaticMeshComponent>();
			UStaticMesh* Mesh = Component ? Component->GetStaticMesh() : nullptr;
			const FMeshDescription* Description = Mesh ? Mesh->GetMeshDescription(0) : nullptr;
			if (!Component || !Description || Description->Vertices().Num() == 0)
			{
				return Fail(
					OutError,
					TEXT("world_spatial_alignment_mesh_vertices_missing"),
					Actor.GetPathName());
			}
			FStaticMeshConstAttributes Attributes(*Description);
			const TVertexAttributesConstRef<FVector3f> Positions = Attributes.GetVertexPositions();
			for (const FVertexID VertexId : Description->Vertices().GetElementIDs())
			{
				OutVertices.Add(Component->GetComponentTransform().TransformPosition(FVector(Positions[VertexId])));
			}
			return !OutVertices.IsEmpty();
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
						OutError);
				}
				if (Request.GetPayload().IsType<FMeshPivotUpdateRequestInput>())
				{
					return TryReadMesh(
						Contract,
						Request.GetPayload().Get<FMeshPivotUpdateRequestInput>().Pivot.Subject,
						Phase,
						Context,
						OutSnapshot,
						OutError);
				}

				TArray<FString> SubjectRefs;
				const FWorldSpatialFrame* QueryFrame = nullptr;
				const FWorldObjectSnapToSurfaceRequestInput* Snap = nullptr;
				if (Request.GetPayload().IsType<FObjectTransformReadRequestInput>())
				{
					if (!Target.IsSet() || Target->GetPublicRef().IsEmpty())
					{
						return Fail(
							OutError,
							TEXT("world_spatial_object_query_target_missing"),
							Contract.GetCapabilityId());
					}
					SubjectRefs.Add(Target->GetPublicRef());
					QueryFrame = &Request.GetPayload()
						.Get<FObjectTransformReadRequestInput>().VerifyFrame;
				}
				else if (Request.GetPayload().IsType<FWorldObjectAlignRequestInput>())
				{
					SubjectRefs = Request.GetPayload()
						.Get<FWorldObjectAlignRequestInput>().SubjectRefs;
				}
				else if (Request.GetPayload().IsType<FWorldObjectSnapToSurfaceRequestInput>())
				{
					Snap = &Request.GetPayload()
						.Get<FWorldObjectSnapToSurfaceRequestInput>();
					SubjectRefs = Snap->SubjectRefs;
				}
				else if (Request.GetPayload().IsType<FTransformCandidatesEvaluateRequestInput>())
				{
					const FTransformCandidatesEvaluateRequestInput& Input = Request.GetPayload()
						.Get<FTransformCandidatesEvaluateRequestInput>();
					SubjectRefs = Input.SubjectRefs;
					QueryFrame = &Input.VerifyFrame;
				}
				else
				{
					return Fail(
						OutError,
						TEXT("world_spatial_object_read_request_mismatch"),
						Contract.GetCapabilityId());
				}

				TArray<FObservedEntry> Entries;
				FString Fingerprint;
				TOptional<FUeAgentStateRevision> TargetRevision;
				const bool bProjectTargetRevision =
					Request.GetPayload().IsType<FObjectTransformReadRequestInput>();
				for (const FString& SubjectRef : SubjectRefs)
				{
					AActor* Actor = nullptr;
					if (!TryResolveActor(*Environment, SubjectRef, Actor, OutError))
					{
						return false;
					}
					if (bProjectTargetRevision)
					{
						FWorldSpatialRevision Revision;
						if (!RevisionSource->TryReadRevision(
								SubjectRef, Revision, OutError))
						{
							return false;
						}
						TargetRevision.Emplace(
							Revision.SourceId,
							Revision.Generation,
							Revision.Sequence);
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
					OutError,
					TargetRevision.IsSet() ? &TargetRevision.GetValue() : nullptr);
			}

			bool TryMutate(
				const FUeAgentWorldSpatialExecutionChainContract& Contract,
				const FUeAgentWorldSpatialRequest& Request,
				const TArray<FUeAgentOperationTargetBinding>& Targets,
				const FUeAgentOperationContext& Context,
				FUeAgentWorldSpatialMutationResult& OutResult,
				FUeAgentWorldSpatialExecutionError& OutError) override
			{
				bool bMutated = false;
				if (Request.GetPayload().IsType<FWorldObjectAlignRequestInput>())
				{
					bMutated = TryAlign(
						Request.GetPayload().Get<FWorldObjectAlignRequestInput>(), OutError);
				}
				else if (Request.GetPayload().IsType<FWorldObjectSnapToSurfaceRequestInput>())
				{
					bMutated = TrySnap(
						Request.GetPayload().Get<FWorldObjectSnapToSurfaceRequestInput>(), OutError);
				}
				else if (Request.GetPayload().IsType<FMeshTransformBakeRequestInput>())
				{
					bMutated = TryBake(
						Request.GetPayload().Get<FMeshTransformBakeRequestInput>(), OutError);
				}
				else if (Request.GetPayload().IsType<FMeshPivotUpdateRequestInput>())
				{
					bMutated = TryPivot(
						Request.GetPayload().Get<FMeshPivotUpdateRequestInput>(), OutError);
				}
				else
				{
					return Fail(
						OutError,
						TEXT("world_spatial_object_mutation_request_mismatch"),
						Contract.GetCapabilityId());
				}
				if (!bMutated)
				{
					return false;
				}
				OutResult = FUeAgentWorldSpatialMutationResult(
					TEXT("world-spatial:") + Context.GetRequestId(),
					TEXT("native-object-mesh-write:") + Contract.GetNativeActionId());
				return OutResult.IsValid();
			}

			bool TrySettle(
				const FUeAgentWorldSpatialExecutionChainContract& Contract,
				const FUeAgentWorldSpatialMutationResult& Mutation,
				const FUeAgentOperationContext& Context,
				FUeAgentWorldSpatialExecutionError& OutError) override
			{
				return Mutation.IsValid()
					? true
					: Fail(
						OutError,
						TEXT("world_spatial_object_mesh_not_settled"),
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
				if (Request.GetPayload().IsType<FWorldObjectAlignRequestInput>())
				{
					bMatches = CompareAlign(
						Request.GetPayload().Get<FWorldObjectAlignRequestInput>(),
						Before,
						After,
						OutError);
				}
				else if (Request.GetPayload().IsType<FWorldObjectSnapToSurfaceRequestInput>())
				{
					bMatches = CompareSnap(
						Request.GetPayload().Get<FWorldObjectSnapToSurfaceRequestInput>(),
						After,
						OutError);
				}
				else if (Request.GetPayload().IsType<FMeshTransformBakeRequestInput>())
				{
					bMatches = CompareBake(
						Request.GetPayload().Get<FMeshTransformBakeRequestInput>(),
						Before,
						After,
						OutError);
				}
				else if (Request.GetPayload().IsType<FMeshPivotUpdateRequestInput>())
				{
					bMatches = ComparePivot(
						Request.GetPayload().Get<FMeshPivotUpdateRequestInput>(),
						Before,
						After,
						OutError);
				}
				else
				{
					return Fail(
						OutError,
						TEXT("world_spatial_object_compare_request_mismatch"),
						Contract.GetCapabilityId());
				}
				if (OutError.IsValid())
				{
					return false;
				}
				SetComparisonOutcome(
					Contract,
					bMatches,
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
				FUeAgentWorldSpatialExecutionError& OutError)
			{
				UStaticMesh* Mesh = nullptr;
				if (!TryResolveMesh(*Environment, MeshRef, Mesh, OutError))
				{
					return false;
				}
				const FMeshDescription* Description = Mesh->GetMeshDescription(0);
				if (!Description || Description->Vertices().Num() == 0)
				{
					return Fail(
						OutError,
						TEXT("world_spatial_mesh_description_missing"),
						MeshRef);
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
					OutError);
			}

			bool TryAlign(
				const FWorldObjectAlignRequestInput& Input,
				FUeAgentWorldSpatialExecutionError& OutError)
			{
				for (const FWorldSpatialExpectedRevisionEntry& Entry : Input.ExpectedRevisions)
				{
					if (!TryCheckRevision(
							*RevisionSource, Entry.TargetRef, Entry.Revision, OutError))
					{
						return false;
					}
				}
				AActor* ReferenceActor = nullptr;
				if (Input.Alignment.Kind != TEXT("anchors"))
				{
					if (!Input.AnchorRef.IsSet()
						|| !TryResolveActor(*Environment, Input.AnchorRef.GetValue(), ReferenceActor, OutError))
					{
						return Input.AnchorRef.IsSet()
							? false
							: Fail(OutError, TEXT("world_spatial_alignment_reference_missing"), Input.Alignment.Kind);
					}
				}

				FUeAgentWorldSpatialCompiledValue ExplicitSubjectAnchor;
				FUeAgentWorldSpatialCompiledValue ExplicitReferenceAnchor;
				FQuat ExplicitRotationDelta = FQuat::Identity;
				if (Input.Alignment.Kind == TEXT("anchors"))
				{
					if (!Input.Alignment.SubjectAnchor.IsSet()
						|| !Input.Alignment.ReferenceAnchor.IsSet()
						|| !SpaceCompiler->TryConvertToNative(
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
							return Fail(OutError, TEXT("world_spatial_alignment_normals_missing"), Input.Alignment.Kind);
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
							if (!TryAxisIndex(Input.Alignment.Axis, SubjectAxis, OutError)) return false;
							ReferenceAxis = SubjectAxis;
						}
						else if (!TryResolveFace(Input.Alignment.SubjectFace, Input.Alignment.Axis, SubjectAxis, SubjectAnchor, OutError)
							|| !TryResolveFace(Input.Alignment.ReferenceFace, Input.Alignment.Axis, ReferenceAxis, ReferenceAnchor, OutError))
						{
							return false;
						}
						if (SubjectAxis != ReferenceAxis)
						{
							return Fail(OutError, TEXT("world_spatial_alignment_face_axis_mismatch"), Input.Alignment.Kind);
						}
						double SubjectValue = 0.0;
						double ReferenceValue = 0.0;
						if (!TryBoundsAnchorValue(Actor->GetComponentsBoundingBox(true), SubjectAxis, SubjectAnchor, SubjectValue, OutError)
							|| !TryBoundsAnchorValue(ReferenceActor->GetComponentsBoundingBox(true), ReferenceAxis, ReferenceAnchor, ReferenceValue, OutError))
						{
							return false;
						}
						Delta = AxisDelta(SubjectAxis, ReferenceValue - SubjectValue + Input.Alignment.OffsetCm);
					}
					else if (Input.Alignment.Kind == TEXT("vertex"))
					{
						TArray<FVector> SubjectVertices;
						TArray<FVector> ReferenceVertices;
						if (!TryActorWorldVertices(*Actor, SubjectVertices, OutError)
							|| !TryActorWorldVertices(*ReferenceActor, ReferenceVertices, OutError))
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
								return Fail(OutError, TEXT("world_spatial_alignment_vertex_pair_incomplete"), Ref);
							}
							SubjectIndex = Input.Alignment.SubjectVertexIndex.GetValue();
							ReferenceIndex = Input.Alignment.ReferenceVertexIndex.GetValue();
							if (!SubjectVertices.IsValidIndex(SubjectIndex)
								|| !ReferenceVertices.IsValidIndex(ReferenceIndex))
							{
								return Fail(OutError, TEXT("world_spatial_alignment_vertex_index_invalid"), Ref);
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
						return Fail(OutError, TEXT("world_spatial_alignment_kind_invalid"), Input.Alignment.Kind);
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
					Planned.Key->Modify();
					Planned.Key->SetActorTransform(
						Planned.Value, false, nullptr, ETeleportType::TeleportPhysics);
				}
				return true;
			}

			bool TrySnap(
				const FWorldObjectSnapToSurfaceRequestInput& Input,
				FUeAgentWorldSpatialExecutionError& OutError)
			{
				for (const FWorldSpatialExpectedRevisionEntry& Entry : Input.ExpectedRevisions)
				{
					if (!TryCheckRevision(
							*RevisionSource, Entry.TargetRef, Entry.Revision, OutError))
					{
						return false;
					}
				}
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

			bool TryBake(
				const FMeshTransformBakeRequestInput& Input,
				FUeAgentWorldSpatialExecutionError& OutError)
			{
				const FString& MeshRef = Input.Transform.Subject;
				if (!TryCheckRevision(
						*RevisionSource, MeshRef, Input.ExpectedRevision, OutError))
				{
					return false;
				}
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
					return Fail(
						OutError,
						TEXT("world_spatial_mesh_description_missing"),
						MeshRef);
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
				for (const FVertexID VertexId : Description->Vertices().GetElementIDs())
				{
					Positions[VertexId] = FVector3f(
						Transform.TransformPosition(FVector(Positions[VertexId])));
				}
				Mesh->CommitMeshDescription(0);
				Mesh->PostEditChange();
				return Mesh->GetMeshDescription(0) != nullptr;
			}

			bool TryPivot(
				const FMeshPivotUpdateRequestInput& Input,
				FUeAgentWorldSpatialExecutionError& OutError)
			{
				const FString& MeshRef = Input.Pivot.Subject;
				if (!TryCheckRevision(
						*RevisionSource, MeshRef, Input.ExpectedRevision, OutError))
				{
					return false;
				}
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
					return Fail(
						OutError,
						TEXT("world_spatial_mesh_description_missing"),
						MeshRef);
				}
				TArray<UStaticMeshComponent*> Components;
				if (Input.PreserveWorldGeometry.Get(false))
				{
					FindMeshComponents(Mesh, Components);
				}
				FStaticMeshAttributes Attributes(*Description);
				TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
				for (const FVertexID VertexId : Description->Vertices().GetElementIDs())
				{
					Positions[VertexId] -= FVector3f(Pivot);
				}
				Mesh->CommitMeshDescription(0);
				Mesh->PostEditChange();
				for (UStaticMeshComponent* Component : Components)
				{
					const FTransform Transform = Component->GetComponentTransform();
					Component->SetWorldLocation(
						Transform.GetLocation() + Transform.TransformVector(Pivot));
				}
				return Mesh->GetMeshDescription(0) != nullptr;
			}

			bool CompareAlign(
				const FWorldObjectAlignRequestInput& Input,
				const FUeAgentCanonicalSnapshot& Before,
				const FUeAgentCanonicalSnapshot& After,
				FUeAgentWorldSpatialExecutionError& OutError)
			{
				if (Input.Alignment.Kind == TEXT("anchors"))
				{
					if (!Input.Alignment.SubjectAnchor.IsSet()
						|| !Input.Alignment.ReferenceAnchor.IsSet())
					{
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
							return false;
						}
					}
					return true;
				}

				if (!Input.AnchorRef.IsSet())
				{
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
							if (!TryAxisIndex(Input.Alignment.Axis, SubjectAxis, OutError)) return false;
							ReferenceAxis = SubjectAxis;
						}
						else if (!TryResolveFace(Input.Alignment.SubjectFace, Input.Alignment.Axis, SubjectAxis, SubjectAnchor, OutError)
							|| !TryResolveFace(Input.Alignment.ReferenceFace, Input.Alignment.Axis, ReferenceAxis, ReferenceAnchor, OutError))
						{
							return false;
						}
						double SubjectValue = 0.0;
						double ReferenceValue = 0.0;
						if (SubjectAxis != ReferenceAxis
							|| !TryBoundsAnchorValue(Actor->GetComponentsBoundingBox(true), SubjectAxis, SubjectAnchor, SubjectValue, OutError)
							|| !TryBoundsAnchorValue(ReferenceActor->GetComponentsBoundingBox(true), ReferenceAxis, ReferenceAnchor, ReferenceValue, OutError)
							|| !FMath::IsNearlyEqual(SubjectValue, ReferenceValue + Input.Alignment.OffsetCm, 0.01))
						{
							return false;
						}
					}
					else if (Input.Alignment.Kind == TEXT("vertex"))
					{
						TArray<FVector> SubjectVertices;
						TArray<FVector> ReferenceVertices;
						if (!TryActorWorldVertices(*Actor, SubjectVertices, OutError)
							|| !TryActorWorldVertices(*ReferenceActor, ReferenceVertices, OutError))
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
							return false;
						}
					}
					else
					{
						return false;
					}
				}
				return true;
			}

			bool CompareSnap(
				const FWorldObjectSnapToSurfaceRequestInput& Input,
				const FUeAgentCanonicalSnapshot& After,
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
						return false;
					}
				}
				return true;
			}

			bool CompareBake(
				const FMeshTransformBakeRequestInput& Input,
				const FUeAgentCanonicalSnapshot& Before,
				const FUeAgentCanonicalSnapshot& After,
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
						return false;
					}
				}
				return true;
			}

			bool ComparePivot(
				const FMeshPivotUpdateRequestInput& Input,
				const FUeAgentCanonicalSnapshot& Before,
				const FUeAgentCanonicalSnapshot& After,
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

