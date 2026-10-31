#include "Composition/SimpleAuthoring/Families/Modeling/UeAgentModelingSimpleAuthoringExistingEdit.h"

#include "Components/DynamicMeshComponent.h"
#include "Core/StateSemantic/UeAgentStateRevisionJson.h"
#include "DomainAdapters/ModelingGeometryEffect/UeAgentModelingGeometryEffectCapabilities.h"
#include "DomainAdapters/ModelingGeometryEffect/UeAgentModelingGeometryEffectBoundarySemantics.h"
#include "DomainAdapters/ModelingGeometryEffect/UeAgentModelingGeometryEffectBoolean.h"
#include "DomainAdapters/ModelingGeometryEffect/UeAgentModelingGeometryEffectCarrierRevision.h"
#include "DomainAdapters/ModelingGeometryEffect/UeAgentModelingGeometryEffectNormalObservation.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMeshActor.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescriptionToDynamicMesh.h"
#include "DynamicMeshToMeshDescription.h"
#include "StaticMeshAttributes.h"
#include "Misc/SecureHash.h"
#include "PhysicsEngine/BodySetup.h"
#include "Selections/MeshConnectedComponents.h"
#include "UDynamicMesh.h"
#include "Dom/JsonValue.h"
#include "UObject/Class.h"

namespace UE::UAI::Composition::SimpleAuthoring::Modeling::ExistingEdit
{
	namespace
	{
		using namespace
			DomainAdapters::ModelingGeometryEffect;
		using namespace ExecutionMutation;
		using UE::Geometry::FDynamicMesh3;

		constexpr const TCHAR* BaselineField =
			TEXT("__uai_modeling_existing_edit_baseline_v1");
		constexpr const TCHAR* DurablePackageNameField =
			TEXT("__uai_modeling_durable_package_name_v1");
		constexpr const TCHAR* ExpectedMaterialPathField =
			TEXT("__uai_expected_material_path");
		constexpr const TCHAR* NormalSplitTopologyField =
			TEXT("normal_split_topology");
		constexpr const TCHAR* NormalSplitTopologySchema =
			TEXT("uai.modeling.normal_split_topology.v1");
		constexpr const TCHAR* ExpectedComponentsField =
			TEXT("__uai_expected_component_geometry_v1");
		constexpr const TCHAR* ExpectedBooleanStateField = TEXT("__uai_expected_boolean_state_v1");

		TSharedPtr<FJsonObject> BooleanGeometrySummary(const FCarrierState& State)
		{
			auto Summary = MakeShared<FJsonObject>();
			Summary->SetNumberField(TEXT("triangle_count"), State.TriangleCount);
			Summary->SetNumberField(TEXT("vertex_count"), State.VertexCount);
			Summary->SetNumberField(TEXT("boundary_edge_count"), State.BoundaryEdgeCount);
			Summary->SetNumberField(TEXT("component_count"), State.ConnectedComponentCount);
			return Summary;
		}

		void SetIntegerArray(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			const TArray<int32>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> Items;
			Items.Reserve(Values.Num());
			for (const int32 Value : Values)
			{
				Items.Add(MakeShared<FJsonValueNumber>(Value));
			}
			Object->SetArrayField(Field, MoveTemp(Items));
		}

		using UE::UAI::Contract::FDiagnostic;

		FDiagnostic BaselineFailure(const FString& Field, const FString& Expected,
			const TSharedPtr<FJsonValue>& Actual = nullptr, const bool bInternal = false)
		{
			auto Error = FDiagnostic::Failure(TEXT("modeling_geometry_baseline_field_invalid"),
				TEXT("The Modeling field does not satisfy its required value."), TEXT("modeling_capture"));
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = Field;
			Error.ExpectedValue = MakeShared<FJsonValueString>(Expected);
			Error.ActualValuePresent = Actual.IsValid();
			if (bInternal) Error.ActualEvidence = Actual;
			else if (Actual.IsValid() && (Actual->Type == EJson::Array || Actual->Type == EJson::Object))
			{
				auto Summary = MakeShared<FJsonObject>();
				Summary->SetStringField(TEXT("kind"), Actual->Type == EJson::Array ? TEXT("array") : TEXT("object"));
				if (Actual->Type == EJson::Array) Summary->SetNumberField(TEXT("count"), Actual->AsArray().Num());
				Error.ActualValue = MakeShared<FJsonValueObject>(Summary);
			}
			else Error.ActualValue = Actual;
			return Error;
		}

		void LocateBaselineRevision(FDiagnostic& Error)
		{
			Error.SourceKind = TEXT("native_result");
			const FString Suffix = Error.PropertyPath == TEXT("target_revision") ? FString() : Error.PropertyPath;
			Error.PropertyPath = TEXT("baseline/target_revision")
				+ (Suffix.IsEmpty() ? FString() : Suffix.StartsWith(TEXT("/")) ? Suffix : TEXT("/") + Suffix);
		}

		bool ReadString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, FString& OutValue,
			FDiagnostic& OutError, const FString& Path, const bool bInternal = false)
		{
			if (Object.IsValid() && Object->TryGetStringField(Field, OutValue)) return true;
			OutError = BaselineFailure(Path, TEXT("string"), Object.IsValid() ? Object->TryGetField(Field) : nullptr, bInternal);
			return false;
		}

		bool TryReadInteger(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, int32& OutValue,
			FDiagnostic& OutError, const FString& ParentPath)
		{
			double Value = 0.0;
			if (!Object.IsValid() || !Object->TryGetNumberField(Field, Value)
				|| !FMath::IsNearlyEqual(Value, FMath::RoundToDouble(Value)))
			{
				OutError = BaselineFailure(ParentPath + TEXT("/") + Field, TEXT("integer-valued number"),
					Object.IsValid() ? Object->TryGetField(Field) : nullptr);
				return false;
			}
			OutValue = FMath::RoundToInt(Value);
			return true;
		}

		bool TryReadNumberArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field,
			const int32 Count, TArray<double>& OutValues, FDiagnostic& OutError, const FString& ParentPath = FString())
		{
			OutValues.Reset();
			const FString Path = ParentPath.IsEmpty() ? FString(Field) : ParentPath + TEXT("/") + Field;
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Object.IsValid() || !Object->TryGetArrayField(Field, Values) || !Values)
			{
				OutError = BaselineFailure(Path, TEXT("number array"), Object.IsValid() ? Object->TryGetField(Field) : nullptr);
				return false;
			}
			if (Values->Num() != Count)
			{
				OutError = BaselineFailure(Path, FString::Printf(TEXT("array with %d elements"), Count),
					MakeShared<FJsonValueNumber>(Values->Num()));
				return false;
			}
			for (int32 Index = 0; Index < Values->Num(); ++Index)
			{
				const auto& Value = (*Values)[Index];
				if (!Value.IsValid() || Value->Type != EJson::Number)
				{
					OutValues.Reset();
					OutError = BaselineFailure(Path + TEXT("/") + FString::FromInt(Index), TEXT("number"), Value);
					return false;
				}
				OutValues.Add(Value->AsNumber());
			}
			return true;
		}

		FString BuildNormalSplitTopologyFingerprint(
			const TArray<FString>& SplitEndpointKeys)
		{
			FSHA1 Sha1;
			for (const FString& Key : SplitEndpointKeys)
			{
				const FTCHARToUTF8 Utf8(*Key);
				const uint32 ByteCount = static_cast<uint32>(
					Utf8.Length());
				Sha1.Update(
					reinterpret_cast<const uint8*>(&ByteCount),
					sizeof(ByteCount));
				if (ByteCount > 0)
				{
					Sha1.Update(
						reinterpret_cast<const uint8*>(Utf8.Get()),
						ByteCount);
				}
			}
			Sha1.Final();
			uint8 Digest[FSHA1::DigestSize];
			Sha1.GetHash(Digest);
			return BytesToHex(Digest, UE_ARRAY_COUNT(Digest));
		}

		bool ReadMeshState(
			const FDynamicMesh3& Mesh,
			FCarrierState& OutState,
			FDiagnostic& OutError,
			const bool bObserveComponents)
		{
			OutState.VertexCount = Mesh.VertexCount();
			OutState.EdgeCount = Mesh.EdgeCount();
			OutState.TriangleCount = Mesh.TriangleCount();
			const FBoundarySemanticObservation Boundary =
				ObserveBoundarySemantics(Mesh);
			OutState.BoundaryVertexCount = Boundary.VertexCount;
			OutState.BoundaryGeometryFingerprint = Boundary.Fingerprint;
			OutState.NormalObservation = ObserveNormalSemantics(Mesh);
			OutState.NormalSplitEndpointCount =
				OutState.NormalObservation.
					SplitInteriorEdgeEndpointKeys.Num();
			OutState.NormalSplitTopologyFingerprint =
				BuildNormalSplitTopologyFingerprint(
					OutState.NormalObservation.
						SplitInteriorEdgeEndpointKeys);
			OutState.bHasNormalSplitTopologyEvidence = true;
			for (const int32 VertexId : Mesh.VertexIndicesItr())
			{
				OutState.VertexIds.Add(VertexId);
				const FVector3d Position = Mesh.GetVertex(VertexId);
				OutState.Bounds += FVector(Position);
				OutState.VertexChecksum +=
					FMath::RoundToInt64(Position.X * 100.0)
					+ FMath::RoundToInt64(Position.Y * 1000.0)
					+ FMath::RoundToInt64(Position.Z * 10000.0);
			}
			for (const int32 EdgeId : Mesh.EdgeIndicesItr())
			{
				OutState.EdgeIds.Add(EdgeId);
				if (Mesh.IsBoundaryEdge(EdgeId))
				{
					OutState.BoundaryEdgeIds.Add(EdgeId);
					++OutState.BoundaryEdgeCount;
				}
			}
			for (const int32 TriangleId : Mesh.TriangleIndicesItr())
			{
				OutState.TriangleIds.Add(TriangleId);
			}
			UE::Geometry::FMeshConnectedComponents Components(&Mesh);
			Components.FindConnectedTriangles();
			OutState.ConnectedComponentCount = Components.Num();
			if (bObserveComponents)
			{
				for (int32 Index = 0; Index < Components.Num(); ++Index)
				{
					FCarrierState::FComponentObservation Observation;
					const TArray<int32>& Triangles = Components[Index].Indices;
					TSet<int32> Vertices;
					for (const int32 TriangleId : Triangles)
					{
						const auto Triangle = Mesh.GetTriangle(TriangleId);
						for (int32 Corner = 0; Corner < 3; ++Corner)
						{
							Vertices.Add(Triangle[Corner]);
							Observation.Bounds += FVector(Mesh.GetVertex(Triangle[Corner]));
						}
					}
					TArray<FString> TriangleKeys;
					for (const int32 TriangleId : Triangles)
					{
						const auto Triangle = Mesh.GetTriangle(TriangleId);
						FString Corners[3];
						for (int32 Corner = 0; Corner < 3; ++Corner)
						{
							const FVector Position = FVector(Mesh.GetVertex(Triangle[Corner])) - Observation.Bounds.Min;
							// Millimeter shape precision tolerates float MeshDescription round trips.
							Corners[Corner] = FString::Printf(TEXT("%lld,%lld,%lld"),
								FMath::RoundToInt64(Position.X * 10), FMath::RoundToInt64(Position.Y * 10), FMath::RoundToInt64(Position.Z * 10));
						}
						int32 First = 0;
						for (int32 Corner = 1; Corner < 3; ++Corner) if (Corners[Corner] < Corners[First]) First = Corner;
						const int32 Material = Mesh.HasAttributes() && Mesh.Attributes()->HasMaterialID()
							? Mesh.Attributes()->GetMaterialID()->GetValue(TriangleId) : 0;
						TriangleKeys.Add(FString::Printf(TEXT("%d:%s;%s;%s"), Material,
							*Corners[First], *Corners[(First + 1) % 3], *Corners[(First + 2) % 3]));
					}
					TriangleKeys.Sort();
					TriangleKeys.Add(FString::FromInt(Vertices.Num()));
					Observation.ShapeFingerprint = BuildNormalSplitTopologyFingerprint(TriangleKeys);
					OutState.Components.Add(MoveTemp(Observation));
				}
			}
			if (Mesh.HasAttributes())
			{
				const UE::Geometry::FDynamicMeshAttributeSet* Attributes =
					Mesh.Attributes();
				for (int32 LayerIndex = 0;
					LayerIndex < Attributes->NumUVLayers();
					++LayerIndex)
				{
					const UE::Geometry::FDynamicMeshUVOverlay* Uv =
						Attributes->GetUVLayer(LayerIndex);
					FVector2f Minimum = FVector2f::ZeroVector;
					FVector2f Maximum = FVector2f::ZeroVector;
					bool bHasElements = false;
					for (const int32 ElementId
						: Uv->ElementIndicesItr())
					{
						const FVector2f Value =
							Uv->GetElement(ElementId);
						if (!bHasElements)
						{
							Minimum = Value;
							Maximum = Value;
							bHasElements = true;
						}
						else
						{
							Minimum.X =
								FMath::Min(Minimum.X, Value.X);
							Minimum.Y =
								FMath::Min(Minimum.Y, Value.Y);
							Maximum.X =
								FMath::Max(Maximum.X, Value.X);
							Maximum.Y =
								FMath::Max(Maximum.Y, Value.Y);
						}
					}
					OutState.UvElementCounts.Add(Uv->ElementCount());
					OutState.UvMins.Add(Minimum);
					OutState.UvMaxs.Add(Maximum);
					OutState.UvHasElements.Add(bHasElements);
				}
				if (const UE::Geometry::FDynamicMeshNormalOverlay* Normals =
					Attributes->PrimaryNormals())
				{
					OutState.NormalElementCount =
						Normals->ElementCount();
					for (const int32 ElementId
						: Normals->ElementIndicesItr())
					{
						const FVector3f Normal =
							Normals->GetElement(ElementId);
						OutState.NormalChecksum +=
							static_cast<int64>(ElementId + 1)
							* (
								FMath::RoundToInt64(
									Normal.X * 1000.0)
								+ FMath::RoundToInt64(
									Normal.Y * 10000.0)
								+ FMath::RoundToInt64(
									Normal.Z * 100000.0));
					}
				}
				if (const UE::Geometry::FDynamicMeshMaterialAttribute*
					Materials = Attributes->GetMaterialID())
				{
					for (const int32 TriangleId
						: Mesh.TriangleIndicesItr())
					{
						OutState.MaterialIdByTriangle.Add(
							TriangleId,
							Materials->GetValue(TriangleId));
					}
				}
			}
			OutState.GeometryFingerprint = FString::Printf(
				TEXT("mesh:v=%d:t=%d:e=%d:c=%lld"),
				OutState.VertexCount,
				OutState.TriangleCount,
				OutState.EdgeCount,
				OutState.VertexChecksum);
			if (!OutState.Bounds.IsValid || OutState.VertexCount <= 0 || OutState.TriangleCount <= 0)
			{
				const bool bInvalidBounds = !OutState.Bounds.IsValid;
				const bool bNoVertices = OutState.VertexCount <= 0;
				OutError = FDiagnostic::Failure(TEXT("modeling_geometry_mesh_state_invalid"),
					bInvalidBounds ? TEXT("The captured Modeling mesh has no valid bounds.")
						: bNoVertices ? TEXT("The captured Modeling mesh has no vertices.")
						: TEXT("The captured Modeling mesh has no triangles."), TEXT("modeling_capture"));
				OutError.SourceKind = TEXT("native_result");
				OutError.PropertyPath = bInvalidBounds ? TEXT("bounds/is_valid") : bNoVertices ? TEXT("vertex_count") : TEXT("triangle_count");
				OutError.TargetPath = OutState.TargetRef;
				if (bInvalidBounds)
				{
					OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
					OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				}
				else
				{
					OutError.ActualValue = MakeShared<FJsonValueNumber>(bNoVertices ? OutState.VertexCount : OutState.TriangleCount);
					OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("greater than zero"));
				}
				return false;
			}
			return true;
		}

		void ReadCollisionState(
			const UBodySetup* BodySetup,
			FCarrierState& OutState)
		{
			if (!BodySetup)
			{
				return;
			}
			const FKAggregateGeom& Geometry = BodySetup->AggGeom;
			OutState.CollisionBoxCount = Geometry.BoxElems.Num();
			OutState.CollisionSphereCount =
				Geometry.SphereElems.Num();
			OutState.CollisionCapsuleCount =
				Geometry.SphylElems.Num()
				+ Geometry.TaperedCapsuleElems.Num();
			OutState.CollisionConvexCount =
				Geometry.ConvexElems.Num();
			OutState.CollisionShapeCount =
				OutState.CollisionBoxCount
				+ OutState.CollisionSphereCount
				+ OutState.CollisionCapsuleCount
				+ OutState.CollisionConvexCount;
			if (OutState.CollisionShapeCount > 0)
			{
				OutState.CollisionBounds =
					Geometry.CalcAABB(FTransform::Identity);
			}
		}

		void ReadMaterials(
			const TArray<FName>& SlotNames,
			const TArray<UMaterialInterface*>& Materials,
			FCarrierState& OutState)
		{
			OutState.MaterialSlotNames = SlotNames;
			for (UMaterialInterface* Material : Materials)
			{
				OutState.MaterialRefs.Add(
					Material ? Material->GetPathName() : FString());
			}
		}

		TSharedPtr<FJsonObject> BuildBaselineObject(
			const FCarrierState& State)
		{
			const TSharedPtr<FJsonObject> Baseline =
				MakeShared<FJsonObject>();
			Baseline->SetStringField(
				TEXT("target_ref"),
				State.TargetRef);
			Baseline->SetStringField(
				TEXT("carrier_kind"),
				State.CarrierKind);
			if (State.ActorGuid.IsValid())
			{
				Baseline->SetStringField(
					TEXT("actor_guid"),
					State.ActorGuid.ToString(
						EGuidFormats::DigitsWithHyphensLower));
			}
			Baseline->SetObjectField(
				TEXT("target_revision"),
				StateSemantic::FUeAgentStateRevisionJson::ToJson(
					State.TargetRevision.GetValue()));
			Baseline->SetStringField(
				TEXT("geometry_fingerprint"),
				State.GeometryFingerprint);
			Baseline->SetNumberField(
				TEXT("vertex_count"),
				State.VertexCount);
			Baseline->SetNumberField(
				TEXT("triangle_count"),
				State.TriangleCount);
			Baseline->SetNumberField(
				TEXT("edge_count"),
				State.EdgeCount);
			Baseline->SetNumberField(
				TEXT("boundary_edge_count"),
				State.BoundaryEdgeCount);
			Baseline->SetNumberField(
				TEXT("boundary_vertex_count"),
				State.BoundaryVertexCount);
			Baseline->SetStringField(
				TEXT("boundary_geometry_fingerprint"),
				State.BoundaryGeometryFingerprint);
			Baseline->SetNumberField(
				TEXT("vertex_checksum"),
				static_cast<double>(State.VertexChecksum));
			Baseline->SetNumberField(
				TEXT("material_slot_count"),
				State.MaterialSlotCount);
			Baseline->SetNumberField(
				TEXT("connected_component_count"),
				State.ConnectedComponentCount);
			Baseline->SetNumberField(
				TEXT("collision_shape_count"),
				State.CollisionShapeCount);
			Baseline->SetNumberField(
				TEXT("collision_box_count"),
				State.CollisionBoxCount);
			Baseline->SetNumberField(
				TEXT("collision_sphere_count"),
				State.CollisionSphereCount);
			Baseline->SetNumberField(
				TEXT("collision_capsule_count"),
				State.CollisionCapsuleCount);
			Baseline->SetNumberField(
				TEXT("collision_convex_count"),
				State.CollisionConvexCount);
			Baseline->SetArrayField(
				TEXT("bounds_min"),
				{
					MakeShared<FJsonValueNumber>(State.Bounds.Min.X),
					MakeShared<FJsonValueNumber>(State.Bounds.Min.Y),
					MakeShared<FJsonValueNumber>(State.Bounds.Min.Z),
				});
			Baseline->SetArrayField(
				TEXT("bounds_max"),
				{
					MakeShared<FJsonValueNumber>(State.Bounds.Max.X),
					MakeShared<FJsonValueNumber>(State.Bounds.Max.Y),
					MakeShared<FJsonValueNumber>(State.Bounds.Max.Z),
				});
			Baseline->SetBoolField(
				TEXT("collision_bounds_valid"),
				State.CollisionBounds.IsValid != 0);
			const FVector CollisionMin =
				State.CollisionBounds.IsValid
					? State.CollisionBounds.Min
					: FVector::ZeroVector;
			const FVector CollisionMax =
				State.CollisionBounds.IsValid
					? State.CollisionBounds.Max
					: FVector::ZeroVector;
			Baseline->SetArrayField(
				TEXT("collision_bounds_min"),
				{
					MakeShared<FJsonValueNumber>(CollisionMin.X),
					MakeShared<FJsonValueNumber>(CollisionMin.Y),
					MakeShared<FJsonValueNumber>(CollisionMin.Z),
				});
			Baseline->SetArrayField(
				TEXT("collision_bounds_max"),
				{
					MakeShared<FJsonValueNumber>(CollisionMax.X),
					MakeShared<FJsonValueNumber>(CollisionMax.Y),
					MakeShared<FJsonValueNumber>(CollisionMax.Z),
				});
			if (State.bHasNormalSplitTopologyEvidence)
			{
				const FNormalSemanticObservation& Observation =
					State.NormalObservation;
				const TSharedPtr<FJsonObject> Topology =
					MakeShared<FJsonObject>();
				Topology->SetStringField(
					TEXT("schema"),
					NormalSplitTopologySchema);
				Topology->SetNumberField(
					TEXT("interior_edge_count"),
					Observation.InteriorEdgeCount);
				Topology->SetNumberField(
					TEXT("invalid_interior_edge_count"),
					Observation.InvalidInteriorEdgeCount);
				Topology->SetNumberField(
					TEXT("partially_split_interior_edge_count"),
					Observation.PartiallySplitInteriorEdgeCount);
				Topology->SetNumberField(
					TEXT("split_endpoint_count"),
					State.NormalSplitEndpointCount);
				Topology->SetStringField(
					TEXT("split_topology_sha1"),
					State.NormalSplitTopologyFingerprint);
				Baseline->SetObjectField(
					NormalSplitTopologyField,
					Topology);
			}
			return Baseline;
		}

		bool TryReadNormalSplitTopology(const TSharedPtr<FJsonObject>& Baseline,
			FCarrierState& OutState, FDiagnostic& OutError)
		{
			const FString Path = TEXT("baseline/normal_split_topology");
			const TSharedPtr<FJsonObject>* Topology = nullptr;
			if (!Baseline.IsValid())
			{
				OutError = BaselineFailure(TEXT("baseline"), TEXT("object"));
				return false;
			}
			if (!Baseline->HasField(NormalSplitTopologyField)) return true;
			if (!Baseline->TryGetObjectField(NormalSplitTopologyField, Topology) || !Topology || !Topology->IsValid())
			{
				OutError = BaselineFailure(Path, TEXT("object"), Baseline->TryGetField(NormalSplitTopologyField));
				return false;
			}
			FString Schema;
			if (!ReadString(*Topology, TEXT("schema"), Schema, OutError, Path + TEXT("/schema"))) return false;
			if (Schema != NormalSplitTopologySchema)
			{
				OutError = BaselineFailure(Path + TEXT("/schema"), NormalSplitTopologySchema, MakeShared<FJsonValueString>(Schema));
				return false;
			}
			if (!TryReadInteger(*Topology, TEXT("interior_edge_count"), OutState.NormalObservation.InteriorEdgeCount, OutError, Path)
				|| !TryReadInteger(*Topology, TEXT("invalid_interior_edge_count"), OutState.NormalObservation.InvalidInteriorEdgeCount, OutError, Path)
				|| !TryReadInteger(*Topology, TEXT("partially_split_interior_edge_count"), OutState.NormalObservation.PartiallySplitInteriorEdgeCount, OutError, Path)
				|| !TryReadInteger(*Topology, TEXT("split_endpoint_count"), OutState.NormalSplitEndpointCount, OutError, Path)
				|| !ReadString(*Topology, TEXT("split_topology_sha1"), OutState.NormalSplitTopologyFingerprint, OutError, Path + TEXT("/split_topology_sha1"), true))
				return false;
			if (OutState.NormalSplitTopologyFingerprint.IsEmpty())
			{
				OutError = BaselineFailure(Path + TEXT("/split_topology_sha1"), TEXT("nonempty fingerprint"), MakeShared<FJsonValueString>(TEXT("")));
				return false;
			}
			OutState.bHasNormalSplitTopologyEvidence = true;
			return true;
		}

		bool StateChanged(
			const FCarrierState& Before,
			const FCarrierState& After)
		{
			return Before.GeometryFingerprint
					!= After.GeometryFingerprint
				|| !Before.Bounds.Min.Equals(After.Bounds.Min)
				|| !Before.Bounds.Max.Equals(After.Bounds.Max)
				|| Before.BoundaryEdgeCount
					!= After.BoundaryEdgeCount;
		}

		double NumberOr(
			const TSharedPtr<FJsonObject>& Payload,
			const TCHAR* Field,
			const double DefaultValue)
		{
			double Value = DefaultValue;
			Payload->TryGetNumberField(Field, Value);
			return Value;
		}

		int32 IntegerOr(
			const TSharedPtr<FJsonObject>& Payload,
			const TCHAR* Field,
			const int32 DefaultValue)
		{
			return FMath::RoundToInt(
				NumberOr(Payload, Field, DefaultValue));
		}

		double BoxSupport(const FBox& Box, const FVector3d& Direction)
		{
			const FVector Center = Box.GetCenter();
			const FVector Extent = Box.GetExtent();
			return FVector3d(Center).Dot(Direction)
				+ FVector3d(Extent).Dot(Direction.GetAbs());
		}

		bool FixedDirectionBoundsAdvance(
			const TSharedPtr<FJsonObject>& Payload,
			const FCarrierState& Before,
			const FCarrierState& After,
			const double Distance)
		{
			const TSharedPtr<FJsonObject>* DirectionObject = nullptr;
			TArray<double> Vector;
			FDiagnostic ProbeError;
			if (!Payload->TryGetObjectField(
					TEXT("direction"),
					DirectionObject)
				|| !DirectionObject
				|| !DirectionObject->IsValid()
				|| !TryReadNumberArray(
					*DirectionObject,
					TEXT("vector"),
					3,
					Vector, ProbeError))
			{
				return false;
			}
			FVector3d Displacement(
				Vector[0] * Distance,
				Vector[1] * Distance,
				Vector[2] * Distance);
			const double Length = Displacement.Normalize();
			return Length > UE_DOUBLE_SMALL_NUMBER
				&& BoxSupport(After.Bounds, Displacement)
					>= BoxSupport(Before.Bounds, Displacement)
						+ Length - 5.0;
		}

		bool CollisionBoundsMatchMesh(
			const FCarrierState& State)
		{
			return State.CollisionBounds.IsValid
				&& State.Bounds.IsValid
				&& State.CollisionBounds.Min.Equals(
					State.Bounds.Min,
					1.0)
				&& State.CollisionBounds.Max.Equals(
					State.Bounds.Max,
					1.0);
		}

		bool RequestedUvLayerMatches(
			const TSharedPtr<FJsonObject>& Payload,
			const FCarrierState& State)
		{
			const int32 Channel =
				IntegerOr(Payload, TEXT("uv_channel"), -1);
			return State.UvElementCounts.IsValidIndex(Channel)
				&& State.UvMins.IsValidIndex(Channel)
				&& State.UvMaxs.IsValidIndex(Channel)
				&& State.UvHasElements.IsValidIndex(Channel)
				&& State.UvHasElements[Channel]
				&& State.UvElementCounts[Channel] > 0
				&& State.UvMins[Channel].X >= -0.001f
				&& State.UvMins[Channel].Y >= -0.001f
				&& State.UvMaxs[Channel].X <= 1.001f
				&& State.UvMaxs[Channel].Y <= 1.001f;
		}

		bool PreservesNormalSplitTopology(
			const FCarrierState& Before,
			const FCarrierState& After)
		{
			const FNormalSemanticObservation& BeforeObservation =
				Before.NormalObservation;
			const FNormalSemanticObservation& AfterObservation =
				After.NormalObservation;
			return Before.bHasNormalSplitTopologyEvidence
				&& After.bHasNormalSplitTopologyEvidence
				&& BeforeObservation.InteriorEdgeCount
					== AfterObservation.InteriorEdgeCount
				&& BeforeObservation.InvalidInteriorEdgeCount == 0
				&& AfterObservation.InvalidInteriorEdgeCount == 0
				&& BeforeObservation.PartiallySplitInteriorEdgeCount == 0
				&& AfterObservation.PartiallySplitInteriorEdgeCount == 0
				&& Before.NormalSplitEndpointCount
					== After.NormalSplitEndpointCount
				&& Before.NormalSplitTopologyFingerprint
					== After.NormalSplitTopologyFingerprint;
		}

		int64 SubdividedTriangleCount(
			const TSharedPtr<FJsonObject>& Payload,
			const int32 BeforeTriangleCount)
		{
			const int32 Levels = IntegerOr(Payload, TEXT("levels"), 1);
			// Both UE 5.6 tessellators use FUniformTessellate once with
			// TessellationNum = Levels; PN then displaces the new vertices.
			const int64 EdgeSegments = static_cast<int64>(Levels) + 1;
			return static_cast<int64>(BeforeTriangleCount) * EdgeSegments * EdgeSegments;
		}

		bool ComponentGeometryMatches(const TSharedPtr<FJsonObject>& Payload, const FCarrierState& Actual)
		{
			const TArray<TSharedPtr<FJsonValue>>* Expected = nullptr;
			if (!Payload->TryGetArrayField(ExpectedComponentsField, Expected) || !Expected
				|| Expected->Num() != Actual.Components.Num()) return false;
			TMultiMap<FString, int32> ByShape;
			for (int32 Index = 0; Index < Actual.Components.Num(); ++Index)
				ByShape.Add(Actual.Components[Index].ShapeFingerprint, Index);
			for (const auto& Value : *Expected)
			{
				const TSharedPtr<FJsonObject>* Component = nullptr;
				if (!Value.IsValid() || !Value->TryGetObject(Component) || !Component) return false;
				FString Shape;
				TArray<double> Minimum, Maximum;
				FDiagnostic Error;
				if (!(*Component)->TryGetStringField(TEXT("shape"), Shape)
					|| !TryReadNumberArray(*Component, TEXT("min"), 3, Minimum, Error)
					|| !TryReadNumberArray(*Component, TEXT("max"), 3, Maximum, Error)) return false;
				TArray<int32> Candidates;
				ByShape.MultiFind(Shape, Candidates);
				int32 Match = INDEX_NONE;
				for (const int32 Index : Candidates)
				{
					const FBox& Bounds = Actual.Components[Index].Bounds;
					if (Bounds.Min.Equals(FVector(Minimum[0], Minimum[1], Minimum[2]), 0.01)
						&& Bounds.Max.Equals(FVector(Maximum[0], Maximum[1], Maximum[2]), 0.01))
					{ Match = Index; break; }
				}
				if (Match == INDEX_NONE) return false;
				ByShape.RemoveSingle(Shape, Match);
			}
			return ByShape.IsEmpty();
		}

		bool SemanticMatches(
			const FString& OperationKey,
			const TSharedPtr<FJsonObject>& Payload,
			const FCarrierState& Before,
			const FCarrierState& After)
		{
			if (!After.IsValid()
				|| Before.TargetRef != After.TargetRef
				|| Before.CarrierKind != After.CarrierKind)
			{
				return false;
			}
			if (OperationKey == TEXT("remove_components") || OperationKey == TEXT("translate_components"))
			{
				return Before.MaterialSlotCount == After.MaterialSlotCount
					&& ComponentGeometryMatches(Payload, After);
			}
			if (OperationKey == TEXT("add_material_slot"))
			{
				FString SlotName;
				FString ExpectedMaterialPath;
				return Payload->TryGetStringField(
						TEXT("slot_name"),
						SlotName)
					&& Payload->TryGetStringField(
						ExpectedMaterialPathField,
						ExpectedMaterialPath)
					&& After.MaterialSlotCount
						== Before.MaterialSlotCount + 1
					&& After.MaterialSlotNames.Contains(
						FName(*SlotName))
					&& After.MaterialRefs.Contains(
						ExpectedMaterialPath);
			}
			if (OperationKey == TEXT("auto_uv")
				|| OperationKey == TEXT("project_uv"))
			{
				return RequestedUvLayerMatches(Payload, After);
			}
			if (OperationKey == TEXT("recompute_normals"))
			{
				FString Mode;
				bool bPreserveHardEdges = false;
				if (!Payload->TryGetStringField(TEXT("mode"), Mode)
					|| !Payload->TryGetBoolField(
						TEXT("preserve_existing_hard_edges"),
						bPreserveHardEdges))
				{
					return false;
				}
				if (Mode == TEXT("face"))
				{
					return IsCompleteFaceNormalObservation(
						After.NormalObservation);
				}
				if (!IsCompleteWeightedNormalObservation(
						After.NormalObservation))
				{
					return false;
				}
				if (Mode == TEXT("angle_threshold"))
				{
					return MatchesNormalOpeningAngle(
						After.NormalObservation,
						NumberOr(
							Payload,
							TEXT("angle_degrees"),
							30.0));
				}
				if (Mode != TEXT("area_angle_weighted"))
				{
					return false;
				}
				return bPreserveHardEdges
					? PreservesNormalSplitTopology(
						Before,
						After)
					: HasPerVertexNormalTopology(
						After.NormalObservation);
			}
			if (OperationKey == TEXT("assign_material_slot"))
			{
				const int32 SlotIndex =
					IntegerOr(Payload, TEXT("slot_index"), -1);
				const TSharedPtr<FJsonObject>* Selection = nullptr;
				const TArray<TSharedPtr<FJsonValue>>* ElementIds =
					nullptr;
				if (SlotIndex < 0
					|| !Payload->TryGetObjectField(
						TEXT("selection"),
						Selection)
					|| !Selection
					|| !Selection->IsValid()
					|| !(*Selection)->TryGetArrayField(
						TEXT("element_ids"),
						ElementIds)
					|| !ElementIds
					|| ElementIds->IsEmpty())
				{
					return false;
				}
				for (const TSharedPtr<FJsonValue>& Value
					: *ElementIds)
				{
					const int32 TriangleId =
						Value.IsValid()
							? FMath::RoundToInt(Value->AsNumber())
							: INDEX_NONE;
					const int32* MaterialId =
						After.MaterialIdByTriangle.Find(TriangleId);
					if (!MaterialId || *MaterialId != SlotIndex)
					{
						return false;
					}
				}
				return true;
			}
			if (OperationKey == TEXT("generate_convex_collision"))
			{
				FString Mode;
				bool bReplace = true;
				Payload->TryGetStringField(TEXT("mode"), Mode);
				Payload->TryGetBoolField(
					TEXT("replace_existing"),
					bReplace);
				const int32 MaxHulls =
					IntegerOr(Payload, TEXT("max_hulls"), 1);
				const int32 MinimumNewHulls =
					Mode == TEXT("single_hull")
						? 1
						: FMath::Max(
							1,
							FMath::Min(
								Before.ConnectedComponentCount,
								MaxHulls));
				const int32 NewHullCount =
					After.CollisionConvexCount
						- (bReplace
							? 0
							: Before.CollisionConvexCount);
				return MaxHulls > 0
					&& NewHullCount >= MinimumNewHulls
					&& NewHullCount <= MaxHulls
					&& (!bReplace
						|| (After.CollisionBoxCount == 0
							&& After.CollisionSphereCount == 0
							&& After.CollisionCapsuleCount == 0))
					&& CollisionBoundsMatchMesh(After);
			}
			if (OperationKey == TEXT("generate_simple_collision"))
			{
				FString Method;
				bool bReplace = true;
				Payload->TryGetStringField(TEXT("method"), Method);
				Payload->TryGetBoolField(
					TEXT("replace_existing"),
					bReplace);
				const int32 MaxShapes =
					IntegerOr(Payload, TEXT("max_shapes"), 1);
				const int32 BaselineShapeCount =
					bReplace ? 0 : Before.CollisionShapeCount;
				const int32 NewShapeCount =
					After.CollisionShapeCount
						- BaselineShapeCount;
				const int32 ExpectedComponentShapes =
					FMath::Max(
						1,
						FMath::Min(
							Before.ConnectedComponentCount,
							MaxShapes));
				const bool bKindMatches =
					Method == TEXT("aligned_boxes")
						? After.CollisionBoxCount
								- (bReplace
									? 0
									: Before.CollisionBoxCount)
							== ExpectedComponentShapes
						: Method == TEXT("capsules")
							? After.CollisionCapsuleCount
									- (bReplace
										? 0
										: Before.CollisionCapsuleCount)
								== NewShapeCount
							: Method == TEXT("spheres")
								? After.CollisionSphereCount
										- (bReplace
											? 0
											: Before.CollisionSphereCount)
									== NewShapeCount
								: true;
				return MaxShapes > 0
					&& NewShapeCount > 0
					&& NewShapeCount <= MaxShapes
					&& bKindMatches
					&& CollisionBoundsMatchMesh(After);
			}
			if (OperationKey == TEXT("bevel_edges")
				|| OperationKey == TEXT("inset_faces"))
			{
				return After.TriangleCount > Before.TriangleCount
					&& After.VertexCount > Before.VertexCount;
			}
			if (OperationKey == TEXT("boolean"))
			{
				return ComponentGeometryMatches(Payload, After);
			}
			if (OperationKey == TEXT("duplicate_faces"))
			{
				int32 SelectionCount = 1;
				const TSharedPtr<FJsonObject>* Selection = nullptr;
				const TArray<TSharedPtr<FJsonValue>>* ElementIds =
					nullptr;
				if (Payload->TryGetObjectField(
						TEXT("selection"),
						Selection)
					&& Selection
					&& Selection->IsValid()
					&& (*Selection)->TryGetArrayField(
						TEXT("element_ids"),
						ElementIds)
					&& ElementIds)
				{
					SelectionCount = ElementIds->Num();
				}
				TArray<double> Translation;
				FDiagnostic ProbeError;
				const bool bTranslation =
					TryReadNumberArray(
						Payload,
						TEXT("translation_cm"),
						3,
						Translation, ProbeError);
				return After.TriangleCount
						== Before.TriangleCount + SelectionCount
					&& (!bTranslation
						|| Translation[2] <= 0.0
						|| After.Bounds.Max.Z
							>= Before.Bounds.Max.Z
								+ Translation[2] - 5.0);
			}
			if (OperationKey == TEXT("extrude_faces")
				|| OperationKey == TEXT("push_pull_faces"))
			{
				const double Distance = FMath::Abs(
					NumberOr(Payload, TEXT("distance_cm"), 0.0));
				FString DirectionMode;
				Payload->TryGetStringField(
					TEXT("direction_mode"),
					DirectionMode);
				return After.TriangleCount
						>= Before.TriangleCount + 8
					&& StateChanged(Before, After)
					&& (DirectionMode != TEXT("fixed")
						|| FixedDirectionBoundsAdvance(
							Payload,
							Before,
							After,
							NumberOr(
								Payload,
								TEXT("distance_cm"),
								Distance)));
			}
			if (OperationKey == TEXT("fill_holes")
				|| OperationKey == TEXT("weld_edges"))
			{
				return After.BoundaryEdgeCount
					< Before.BoundaryEdgeCount;
			}
			if (OperationKey == TEXT("mesh_cut"))
			{
				FString KeepSides;
				bool bFillCut = false;
				Payload->TryGetStringField(
					TEXT("keep_sides"),
					KeepSides);
				Payload->TryGetBoolField(TEXT("fill_cut"), bFillCut);
				if (!StateChanged(Before, After)
					|| After.TriangleCount == 0)
				{
					return false;
				}
				if (KeepSides == TEXT("both"))
				{
					return After.BoundaryEdgeCount
							== Before.BoundaryEdgeCount
						&& After.TriangleCount > Before.TriangleCount;
				}
				return bFillCut
					? After.BoundaryEdgeCount <= Before.BoundaryEdgeCount
					: After.BoundaryEdgeCount > Before.BoundaryEdgeCount;
			}
			if (OperationKey == TEXT("plane_cut"))
			{
				FString KeepSide;
				bool bFillHole = false;
				Payload->TryGetStringField(TEXT("keep_side"), KeepSide);
				Payload->TryGetBoolField(TEXT("fill_hole"), bFillHole);
				if (!StateChanged(Before, After)
					|| After.TriangleCount == 0)
				{
					return false;
				}
				const bool bBoundaryMatches = bFillHole
					? After.BoundaryEdgeCount <= Before.BoundaryEdgeCount
					: After.BoundaryEdgeCount > Before.BoundaryEdgeCount;
				return bBoundaryMatches
					&& (KeepSide != TEXT("both")
						|| After.TriangleCount > Before.TriangleCount);
			}
			if (OperationKey == TEXT("mirror"))
			{
				bool bKeepOriginal = true;
				bool bWeldSeam = false;
				Payload->TryGetBoolField(
					TEXT("keep_original"),
					bKeepOriginal);
				Payload->TryGetBoolField(
					TEXT("weld_seam"),
					bWeldSeam);
				if (!bKeepOriginal)
				{
					return StateChanged(Before, After)
						&& After.TriangleCount == Before.TriangleCount
						&& After.BoundaryEdgeCount
							== Before.BoundaryEdgeCount;
				}
				if (After.TriangleCount < Before.TriangleCount * 2)
				{
					return false;
				}
				return bWeldSeam
					? (Before.BoundaryEdgeCount == 0
						? After.BoundaryEdgeCount == 0
						: After.BoundaryEdgeCount
							< Before.BoundaryEdgeCount)
					: After.BoundaryEdgeCount
						>= Before.BoundaryEdgeCount * 2;
			}
			if (OperationKey == TEXT("offset_faces"))
			{
				const double Distance =
					NumberOr(
						Payload,
						TEXT("distance_cm"),
						0.0);
				return FMath::Abs(Distance) > UE_DOUBLE_SMALL_NUMBER
					&& StateChanged(Before, After)
					&& After.VertexCount > Before.VertexCount
					&& After.TriangleCount >= Before.TriangleCount + 8;
			}
			if (OperationKey == TEXT("remesh"))
			{
				return After.VertexCount > 0
					&& After.EdgeCount > 0
					&& After.TriangleCount > 0
					&& StateChanged(Before, After);
			}
			if (OperationKey == TEXT("simplify"))
			{
				FString TargetMode;
				bool bPreserveBoundaries = false;
				Payload->TryGetStringField(
					TEXT("target_mode"),
					TargetMode);
				Payload->TryGetBoolField(
					TEXT("preserve_boundaries"),
					bPreserveBoundaries);
				if (bPreserveBoundaries
					&& (After.BoundaryVertexCount
							!= Before.BoundaryVertexCount
						|| After.BoundaryGeometryFingerprint
							!= Before.BoundaryGeometryFingerprint))
				{
					return false;
				}
				const double TargetValue =
					NumberOr(
						Payload,
						TEXT("target_value"),
						1.0);
				if (TargetMode == TEXT("triangle_count"))
				{
					return After.TriangleCount
						<= FMath::RoundToInt(TargetValue) + 1;
				}
				if (TargetMode == TEXT("triangle_ratio"))
				{
					return After.TriangleCount
						<= FMath::CeilToInt(
							Before.TriangleCount * TargetValue)
								+ 1;
				}
				return TargetMode == TEXT("tolerance_cm")
					&& StateChanged(Before, After)
					&& After.TriangleCount <= Before.TriangleCount;
			}
			if (OperationKey == TEXT("subdivide"))
			{
				return IntegerOr(Payload, TEXT("levels"), 1) > 0
					&& After.TriangleCount
						== SubdividedTriangleCount(Payload, Before.TriangleCount);
			}
			if (OperationKey == TEXT("trim"))
			{
				bool bFillHole = false;
				Payload->TryGetBoolField(TEXT("fill_hole"), bFillHole);
				return After.TriangleCount > 0
					&& StateChanged(Before, After)
					&& (bFillHole
						? After.BoundaryEdgeCount
							<= Before.BoundaryEdgeCount
						: After.BoundaryEdgeCount
							> Before.BoundaryEdgeCount);
			}
			if (OperationKey == TEXT("voxel_boolean"))
			{
				return After.BoundaryEdgeCount == 0
					&& After.TriangleCount > Before.TriangleCount;
			}
			return false;
		}

		TSharedPtr<FJsonObject> BuildVerificationState(
			const FString& OperationKey,
			const FCarrierState& Baseline,
			const bool bSemanticMatches)
		{
			const TSharedPtr<FJsonObject> State =
				MakeShared<FJsonObject>();
			State->SetStringField(
				TEXT("target_ref"),
				Baseline.TargetRef);
			State->SetStringField(
				TEXT("carrier_kind"),
				Baseline.CarrierKind);
			State->SetStringField(
				TEXT("operation_key"),
				OperationKey);
			State->SetBoolField(
				TEXT("semantic_matches"),
				bSemanticMatches);
			return State;
		}
	}

	bool FCarrierState::IsValid() const
	{
		FDiagnostic Error;
		return TryValidate(Error);
	}

	bool FCarrierState::TryValidate(FDiagnostic& OutError) const
	{
		OutError.Reset();
		const auto RequireText = [&](const FString& Value, const TCHAR* Field)
		{
			if (!Value.IsEmpty()) return true;
			OutError = BaselineFailure(Field, TEXT("nonempty string"), MakeShared<FJsonValueString>(Value));
			OutError.TargetPath = TargetRef;
			return false;
		};
		if (!RequireText(TargetRef, TEXT("baseline/target_ref"))
			|| !RequireText(DurablePackageName, TEXT("durable_package_name"))) return false;
		if (!TargetRevision.IsSet())
		{
			OutError = BaselineFailure(TEXT("baseline/target_revision"), TEXT("state revision"));
			OutError.TargetPath = TargetRef;
			return false;
		}
		if (!TargetRevision->IsValid())
		{
			OutError = TargetRevision->GetValidationDiagnostic();
			LocateBaselineRevision(OutError);
			if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
			return false;
		}
		if (!RequireText(CarrierKind, TEXT("baseline/carrier_kind"))) return false;
		if (CarrierKind == TEXT("dynamic_mesh_actor") && !ActorGuid.IsValid())
		{
			OutError = BaselineFailure(TEXT("baseline/actor_guid"), TEXT("nonzero actor GUID"),
				MakeShared<FJsonValueString>(ActorGuid.ToString()), true);
			OutError.TargetPath = TargetRef;
			return false;
		}
		if (!RequireText(GeometryFingerprint, TEXT("baseline/geometry_fingerprint"))
			|| !RequireText(BoundaryGeometryFingerprint, TEXT("baseline/boundary_geometry_fingerprint"))) return false;
		if (!Bounds.IsValid)
		{
			OutError = BaselineFailure(TEXT("baseline/bounds"), TEXT("initialized bounds"));
			OutError.TargetPath = TargetRef;
			return false;
		}
		if (VertexCount <= 0 || TriangleCount <= 0)
		{
			OutError = BaselineFailure(VertexCount <= 0 ? TEXT("baseline/vertex_count") : TEXT("baseline/triangle_count"),
				TEXT("positive count"), MakeShared<FJsonValueNumber>(VertexCount <= 0 ? VertexCount : TriangleCount));
			OutError.TargetPath = TargetRef;
			return false;
		}
		return true;
	}

	bool TryReadCarrier(
		UWorld& World,
		IUeAgentModelingGeometryEffectReferenceResolver& Resolver,
		const FString& TargetRef,
		FCarrierState& OutState,
		UE::UAI::Contract::FDiagnostic& OutError,
		const bool bObserveComponents,
		FDynamicMesh3* OutMesh)
	{
		OutState = FCarrierState();
		OutError.Reset();
		UObject* Object = nullptr;
		FExecutionMutationError Error;
		if (TargetRef.IsEmpty())
		{
			OutError = FDiagnostic::Failure(TEXT("modeling_geometry_target_unresolved"),
				TEXT("The Modeling capture target reference is empty."), TEXT("modeling_capture"));
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = TEXT("target_ref");
			OutError.ActualValue = MakeShared<FJsonValueString>(TargetRef);
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("nonempty public reference"));
			return false;
		}
		const bool bResolved = Resolver.TryResolveReference(World, TargetRef, Object, Error);
		if (!bResolved || !Object)
		{
			OutError = Error;
			if (OutError.IsEmpty())
			{
				OutError = FDiagnostic::Failure(TEXT("modeling_geometry_target_unresolved"),
					bResolved ? TEXT("The Modeling reference resolver reported success without a target object.")
						: TEXT("The Modeling reference resolver failed without returning a diagnostic."), TEXT("modeling_capture"));
				OutError.SourceKind = TEXT("native_result");
				OutError.PropertyPath = TEXT("target_ref");
				OutError.ActualValuePresent = Object != nullptr;
				OutError.ExpectedValuePresent = true;
			}
			if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
			return false;
		}

		OutState.TargetRef = TargetRef;
		OutState.DurablePackageName =
			Object->GetOutermost()->GetName();
		if (!TryReadModelingGeometryCarrierRevision(
				TargetRef,
				*Object,
				OutState.TargetRevision,
				Error))
		{
			OutError = MoveTemp(Error);
			return false;
		}
		if (ADynamicMeshActor* Actor =
			Cast<ADynamicMeshActor>(Object))
		{
			OutState.ActorGuid = Actor->GetActorGuid();
			if (!OutState.ActorGuid.IsValid())
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("modeling_geometry_actor_guid_invalid"), TEXT("The dynamic mesh actor has no valid actor identity."), TEXT("modeling_capture"));
				OutError.TargetPath = Actor->GetPathName();
				return false;
			}
			UDynamicMeshComponent* Component =
				Actor->GetDynamicMeshComponent();
			UDynamicMesh* DynamicMesh =
				Component ? Component->GetDynamicMesh() : nullptr;
			if (!DynamicMesh)
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("modeling_geometry_dynamic_mesh_missing"), TEXT("The actor has no dynamic mesh."), TEXT("modeling_capture"));
				return false;
			}
			OutState.CarrierKind =
				TEXT("dynamic_mesh_actor");
			OutState.MaterialSlotCount =
				Component->GetNumMaterials();
			TArray<UMaterialInterface*> Materials;
			Materials.Reserve(OutState.MaterialSlotCount);
			for (int32 Index = 0;
				Index < OutState.MaterialSlotCount;
				++Index)
			{
				Materials.Add(Component->GetMaterial(Index));
			}
			ReadMaterials(
				Component->GetMaterialSlotNames(),
				Materials,
				OutState);
			ReadCollisionState(
				Component->GetBodySetup(),
				OutState);
			bool bRead = false;
			DynamicMesh->ProcessMesh(
				[&](const FDynamicMesh3& Mesh)
				{
					if (OutMesh) *OutMesh = Mesh;
					bRead = ReadMeshState(Mesh, OutState, OutError, bObserveComponents);
				});
			if (!bRead)
			{
				if (OutError.IsEmpty())
				{
					OutError = FDiagnostic::Failure(TEXT("modeling_geometry_dynamic_mesh_read_failed"),
						TEXT("The dynamic mesh read produced neither a state nor a diagnostic."), TEXT("modeling_capture"));
					OutError.SourceKind = TEXT("native_result");
				}
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
				return false;
			}
			return true;
		}

		if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(Object))
		{
			const FMeshDescription* Description =
				StaticMesh->GetMeshDescription(0);
			if (!Description)
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("modeling_geometry_static_mesh_source_missing"), TEXT("The static mesh has no source mesh description."), TEXT("modeling_capture"));
				return false;
			}
			FDynamicMesh3 Mesh;
			FMeshDescriptionToDynamicMesh Converter;
			Converter.Convert(Description, Mesh);
			if (OutMesh) *OutMesh = Mesh;
			OutState.CarrierKind =
				TEXT("static_mesh_asset");
			OutState.MaterialSlotCount =
				StaticMesh->GetStaticMaterials().Num();
			TArray<FName> SlotNames;
			TArray<UMaterialInterface*> Materials;
			SlotNames.Reserve(OutState.MaterialSlotCount);
			Materials.Reserve(OutState.MaterialSlotCount);
			for (const FStaticMaterial& Material
				: StaticMesh->GetStaticMaterials())
			{
				SlotNames.Add(Material.MaterialSlotName);
				Materials.Add(Material.MaterialInterface);
			}
			ReadMaterials(SlotNames, Materials, OutState);
			ReadCollisionState(
				StaticMesh->GetBodySetup(),
				OutState);
			if (!ReadMeshState(Mesh, OutState, OutError, bObserveComponents))
			{
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetRef;
				return false;
			}
			return true;
		}

		OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("modeling_geometry_carrier_kind_unsupported"), TEXT("The target is not a supported dynamic mesh actor or static mesh asset."), TEXT("modeling_capture"));
		OutError.TargetPath = Object->GetPathName();
		OutError.InternalContext = Object->GetClass()->GetPathName();
		return false;
	}

	TSharedPtr<FJsonObject> BuildDraftState(
		const FCarrierState& State)
	{
		const TSharedPtr<FJsonObject> Object =
			MakeShared<FJsonObject>();
		Object->SetStringField(
			TEXT("target_ref"),
			State.TargetRef);
		Object->SetObjectField(
			TEXT("target_revision"),
			StateSemantic::FUeAgentStateRevisionJson::ToJson(
				State.TargetRevision.GetValue()));
		Object->SetStringField(
			TEXT("carrier_kind"),
			State.CarrierKind);
		if (State.ActorGuid.IsValid())
		{
			Object->SetStringField(
				TEXT("actor_guid"),
				State.ActorGuid.ToString(
					EGuidFormats::DigitsWithHyphensLower));
		}
		Object->SetStringField(
			TEXT("geometry_fingerprint"),
			State.GeometryFingerprint);
		Object->SetStringField(
			TEXT("boundary_geometry_fingerprint"),
			State.BoundaryGeometryFingerprint);
		Object->SetNumberField(
			TEXT("boundary_vertex_count"),
			State.BoundaryVertexCount);
		SetIntegerArray(
			Object,
			TEXT("vertex_ids"),
			State.VertexIds);
		SetIntegerArray(
			Object,
			TEXT("edge_ids"),
			State.EdgeIds);
		SetIntegerArray(
			Object,
			TEXT("boundary_edge_ids"),
			State.BoundaryEdgeIds);
		SetIntegerArray(
			Object,
			TEXT("triangle_ids"),
			State.TriangleIds);
		Object->SetNumberField(
			TEXT("material_slot_count"),
			State.MaterialSlotCount);
		return Object;
	}

	bool TryAttachBaselineEvidence(
		UWorld& World,
		IUeAgentModelingGeometryEffectReferenceResolver& Resolver,
		const FString& OperationKey,
		const FCarrierState& State,
		const TSharedPtr<FJsonObject>& Payload,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		if (!State.TryValidate(OutError)) return false;
		if (!Payload.IsValid())
		{
			OutError = BaselineFailure(TEXT("evidence"), TEXT("object"));
			OutError.TargetPath = State.TargetRef;
			return false;
		}
		Payload->SetObjectField(
			BaselineField,
			BuildBaselineObject(State));
		Payload->SetStringField(
			DurablePackageNameField,
			State.DurablePackageName);
		if (OperationKey == TEXT("boolean"))
		{
			FString OperandRef, Operation;
			const TSharedPtr<FJsonObject>* Transform = nullptr;
			bool bSimplifyOutput = false;
			TArray<double> Translation, Rotation, Scale;
			if (!ReadString(Payload, TEXT("operand_ref"), OperandRef, OutError, TEXT("operand_ref"))
				|| !ReadString(Payload, TEXT("operation"), Operation, OutError, TEXT("operation"))) return false;
			if (!Payload->TryGetObjectField(TEXT("operand_to_target"), Transform) || !Transform
				|| !Payload->TryGetBoolField(TEXT("simplify_output"), bSimplifyOutput))
			{
				OutError = BaselineFailure(TEXT("boolean"), TEXT("operand transform and simplify_output"));
				return false;
			}
			if (!TryReadNumberArray(*Transform, TEXT("translation"), 3, Translation, OutError)
				|| !TryReadNumberArray(*Transform, TEXT("rotation_quaternion_xyzw"), 4, Rotation, OutError)
				|| !TryReadNumberArray(*Transform, TEXT("scale"), 3, Scale, OutError)) return false;
			FDynamicMesh3 TargetMesh, OperandMesh, ExpectedMesh;
			FCarrierState TargetState, OperandState, ExpectedState;
			if (!TryReadCarrier(World, Resolver, State.TargetRef, TargetState, OutError, false, &TargetMesh)
				|| !TryReadCarrier(World, Resolver, OperandRef, OperandState, OutError, false, &OperandMesh)) return false;
			const FTransform OperandToTarget(FQuat(Rotation[0], Rotation[1], Rotation[2], Rotation[3]),
				FVector(Translation[0], Translation[1], Translation[2]), FVector(Scale[0], Scale[1], Scale[2]));
			FExecutionMutationError NativeError;
			if (!TryComputeModelingBoolean(TargetMesh, OperandMesh, OperandToTarget, Operation,
				bSimplifyOutput, ExpectedMesh, NativeError)) { OutError = NativeError; return false; }
			// Observe the same native source representation as CommitStaticMesh:
			// MeshDescription also canonicalizes polygon/material attributes.
			if (State.CarrierKind == TEXT("static_mesh_asset"))
			{
				FMeshDescription Description;
				FStaticMeshAttributes Attributes(Description);
				Attributes.Register();
				FDynamicMeshToMeshDescription ToDescription;
				ToDescription.Convert(&ExpectedMesh, Description);
				FDynamicMesh3 SourceMesh;
				FMeshDescriptionToDynamicMesh FromDescription;
				FromDescription.Convert(&Description, SourceMesh);
				ExpectedMesh = MoveTemp(SourceMesh);
			}
			if (!ReadMeshState(ExpectedMesh, ExpectedState, OutError, true)) return false;
			TArray<TSharedPtr<FJsonValue>> Expected;
			for (const auto& Component : ExpectedState.Components)
			{
				auto Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("shape"), Component.ShapeFingerprint);
				TArray<TSharedPtr<FJsonValue>> Minimum, Maximum;
				for (int32 Axis = 0; Axis < 3; ++Axis)
				{
					Minimum.Add(MakeShared<FJsonValueNumber>(Component.Bounds.Min[Axis]));
					Maximum.Add(MakeShared<FJsonValueNumber>(Component.Bounds.Max[Axis]));
				}
				Item->SetArrayField(TEXT("min"), Minimum);
				Item->SetArrayField(TEXT("max"), Maximum);
				Expected.Add(MakeShared<FJsonValueObject>(Item));
			}
			Payload->SetArrayField(ExpectedComponentsField, Expected);
			Payload->SetObjectField(ExpectedBooleanStateField, BooleanGeometrySummary(ExpectedState));
		}
		if (OperationKey == TEXT("remove_components") || OperationKey == TEXT("translate_components"))
		{
			TArray<double> Minimum, Maximum, Translation;
			if (!TryReadNumberArray(Payload, TEXT("bounds_min_cm"), 3, Minimum, OutError)
				|| !TryReadNumberArray(Payload, TEXT("bounds_max_cm"), 3, Maximum, OutError)) return false;
			const bool bTranslate = OperationKey == TEXT("translate_components");
			if (bTranslate && !TryReadNumberArray(Payload, TEXT("translation_cm"), 3, Translation, OutError)) return false;
			TArray<TSharedPtr<FJsonValue>> Expected;
			for (const auto& Component : State.Components)
			{
				bool bContained = true;
				for (int32 Axis = 0; Axis < 3; ++Axis)
					bContained &= Component.Bounds.Min[Axis] >= Minimum[Axis] && Component.Bounds.Max[Axis] <= Maximum[Axis];
				if (bContained && !bTranslate) continue;
				FBox Bounds = Component.Bounds;
				if (bContained) Bounds = Bounds.ShiftBy(FVector(Translation[0], Translation[1], Translation[2]));
				auto Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("shape"), Component.ShapeFingerprint);
				TArray<TSharedPtr<FJsonValue>> MinValues, MaxValues;
				for (int32 Axis = 0; Axis < 3; ++Axis)
				{
					MinValues.Add(MakeShared<FJsonValueNumber>(Bounds.Min[Axis]));
					MaxValues.Add(MakeShared<FJsonValueNumber>(Bounds.Max[Axis]));
				}
				Item->SetArrayField(TEXT("min"), MinValues);
				Item->SetArrayField(TEXT("max"), MaxValues);
				Expected.Add(MakeShared<FJsonValueObject>(Item));
			}
			Payload->SetArrayField(ExpectedComponentsField, Expected);
		}
		if (OperationKey == TEXT("add_material_slot"))
		{
			FString MaterialRef;
			UObject* MaterialObject = nullptr;
			FExecutionMutationError Error;
			if (!ReadString(Payload, TEXT("material_ref"), MaterialRef, OutError, TEXT("material_ref")))
			{
				OutError.SourceKind = TEXT("command_input");
				OutError.Source.JsonPath = TEXT("/material_ref");
				return false;
			}
			const bool bResolved = Resolver.TryResolveReference(World, MaterialRef, MaterialObject, Error);
			if (!bResolved || !Cast<UMaterialInterface>(MaterialObject))
			{
				OutError = Error;
				if (OutError.IsEmpty())
				{
					OutError = FDiagnostic::Failure(TEXT("modeling_geometry_material_reference_invalid"),
						!bResolved ? TEXT("The Modeling material resolver failed without a diagnostic.")
							: TEXT("The Modeling material reference did not resolve to a material interface."), TEXT("modeling_capture"));
					OutError.SourceKind = TEXT("native_result");
					OutError.PropertyPath = TEXT("material_ref");
					OutError.ActualValuePresent = MaterialObject != nullptr;
					if (MaterialObject) OutError.ActualValue = MakeShared<FJsonValueString>(MaterialObject->GetClass()->GetPathName());
					OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("/Script/Engine.MaterialInterface"));
				}
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = MaterialRef;
				return false;
			}
			Payload->SetStringField(
				ExpectedMaterialPathField,
				MaterialObject->GetPathName());
		}
		return true;
	}

	bool TryReadBaseline(const TSharedPtr<FJsonObject>& Payload, FCarrierState& OutState, FDiagnostic& OutError)
	{
		OutState = FCarrierState();
		OutError.Reset();
		if (!Payload.IsValid())
		{
			OutError = BaselineFailure(TEXT("evidence"), TEXT("object"));
			return false;
		}
		if (!TryReadDurablePackageNameEvidence(Payload, OutState.DurablePackageName, OutError)) return false;
		const TSharedPtr<FJsonObject>* Baseline = nullptr;
		if (!Payload->TryGetObjectField(BaselineField, Baseline) || !Baseline || !Baseline->IsValid())
		{
			OutError = BaselineFailure(TEXT("baseline"), TEXT("object"), Payload->TryGetField(BaselineField));
			return false;
		}
		if (!ReadString(*Baseline, TEXT("target_ref"), OutState.TargetRef, OutError, TEXT("baseline/target_ref"))) return false;
		const TSharedPtr<FJsonObject>* TargetRevision = nullptr;
		if (!(*Baseline)->TryGetObjectField(TEXT("target_revision"), TargetRevision) || !TargetRevision)
		{
			OutError = BaselineFailure(TEXT("baseline/target_revision"), TEXT("object"), (*Baseline)->TryGetField(TEXT("target_revision")), true);
			return false;
		}
		if (!StateSemantic::FUeAgentStateRevisionJson::TryParse(*TargetRevision, OutState.TargetRevision, OutError))
		{
			LocateBaselineRevision(OutError);
			return false;
		}
		double VertexChecksum = 0.0;
		const auto ReadChecksum = [&]()
		{
			if ((*Baseline)->TryGetNumberField(TEXT("vertex_checksum"), VertexChecksum)) return true;
			OutError = BaselineFailure(TEXT("baseline/vertex_checksum"), TEXT("number"), (*Baseline)->TryGetField(TEXT("vertex_checksum")), true);
			return false;
		};
		if (!ReadString(*Baseline, TEXT("carrier_kind"), OutState.CarrierKind, OutError, TEXT("baseline/carrier_kind"))
			|| !ReadString(*Baseline, TEXT("geometry_fingerprint"), OutState.GeometryFingerprint, OutError, TEXT("baseline/geometry_fingerprint"), true)
			|| !TryReadInteger(*Baseline, TEXT("vertex_count"), OutState.VertexCount, OutError, TEXT("baseline"))
			|| !TryReadInteger(*Baseline, TEXT("triangle_count"), OutState.TriangleCount, OutError, TEXT("baseline"))
			|| !TryReadInteger(*Baseline, TEXT("edge_count"), OutState.EdgeCount, OutError, TEXT("baseline"))
			|| !TryReadInteger(*Baseline, TEXT("boundary_edge_count"), OutState.BoundaryEdgeCount, OutError, TEXT("baseline"))
			|| !TryReadInteger(*Baseline, TEXT("boundary_vertex_count"), OutState.BoundaryVertexCount, OutError, TEXT("baseline"))
			|| !ReadString(*Baseline, TEXT("boundary_geometry_fingerprint"), OutState.BoundaryGeometryFingerprint, OutError, TEXT("baseline/boundary_geometry_fingerprint"), true)
			|| !ReadChecksum()
			|| !TryReadInteger(*Baseline, TEXT("material_slot_count"), OutState.MaterialSlotCount, OutError, TEXT("baseline"))
			|| !TryReadInteger(*Baseline, TEXT("connected_component_count"), OutState.ConnectedComponentCount, OutError, TEXT("baseline"))
			|| !TryReadInteger(*Baseline, TEXT("collision_shape_count"), OutState.CollisionShapeCount, OutError, TEXT("baseline"))
			|| !TryReadInteger(*Baseline, TEXT("collision_box_count"), OutState.CollisionBoxCount, OutError, TEXT("baseline"))
			|| !TryReadInteger(*Baseline, TEXT("collision_sphere_count"), OutState.CollisionSphereCount, OutError, TEXT("baseline"))
			|| !TryReadInteger(*Baseline, TEXT("collision_capsule_count"), OutState.CollisionCapsuleCount, OutError, TEXT("baseline"))
			|| !TryReadInteger(*Baseline, TEXT("collision_convex_count"), OutState.CollisionConvexCount, OutError, TEXT("baseline")))
			return false;
		TArray<double> BoundsMin, BoundsMax, CollisionBoundsMin, CollisionBoundsMax;
		if (!TryReadNumberArray(*Baseline, TEXT("bounds_min"), 3, BoundsMin, OutError, TEXT("baseline"))
			|| !TryReadNumberArray(*Baseline, TEXT("bounds_max"), 3, BoundsMax, OutError, TEXT("baseline"))) return false;
		bool bCollisionBoundsValid = false;
		if (!(*Baseline)->TryGetBoolField(TEXT("collision_bounds_valid"), bCollisionBoundsValid))
		{
			OutError = BaselineFailure(TEXT("baseline/collision_bounds_valid"), TEXT("boolean"), (*Baseline)->TryGetField(TEXT("collision_bounds_valid")));
			return false;
		}
		if (!TryReadNumberArray(*Baseline, TEXT("collision_bounds_min"), 3, CollisionBoundsMin, OutError, TEXT("baseline"))
			|| !TryReadNumberArray(*Baseline, TEXT("collision_bounds_max"), 3, CollisionBoundsMax, OutError, TEXT("baseline"))
			|| !TryReadNormalSplitTopology(*Baseline, OutState, OutError)) return false;
		if (OutState.CarrierKind == TEXT("dynamic_mesh_actor"))
		{
			FString ActorGuid;
			if (!ReadString(*Baseline, TEXT("actor_guid"), ActorGuid, OutError, TEXT("baseline/actor_guid"), true)) return false;
			if (!FGuid::Parse(ActorGuid, OutState.ActorGuid) || !OutState.ActorGuid.IsValid())
			{
				OutError = BaselineFailure(TEXT("baseline/actor_guid"), TEXT("nonzero actor GUID"), MakeShared<FJsonValueString>(ActorGuid), true);
				return false;
			}
		}
		OutState.VertexChecksum = FMath::RoundToInt64(VertexChecksum);
		OutState.Bounds = FBox(FVector(BoundsMin[0], BoundsMin[1], BoundsMin[2]), FVector(BoundsMax[0], BoundsMax[1], BoundsMax[2]));
		if (bCollisionBoundsValid)
			OutState.CollisionBounds = FBox(FVector(CollisionBoundsMin[0], CollisionBoundsMin[1], CollisionBoundsMin[2]),
				FVector(CollisionBoundsMax[0], CollisionBoundsMax[1], CollisionBoundsMax[2]));
		return OutState.TryValidate(OutError);
	}

	bool TryReadDurablePackageNameEvidence(const TSharedPtr<FJsonObject>& Payload,
		FString& OutPackageName, FDiagnostic& OutError)
	{
		OutPackageName.Reset();
		if (Payload.IsValid() && Payload->TryGetStringField(DurablePackageNameField, OutPackageName) && !OutPackageName.IsEmpty()) return true;
		OutError = BaselineFailure(TEXT("durable_package_name"), TEXT("nonempty package name"),
			Payload.IsValid() ? Payload->TryGetField(DurablePackageNameField) : nullptr);
		return false;
	}

	TSharedPtr<FJsonObject> BuildExpectedVerificationState(
		const FString& OperationKey,
		const FCarrierState& Baseline,
		const TSharedPtr<FJsonObject>& Payload)
	{
		TSharedPtr<FJsonObject> State = BuildVerificationState(
			OperationKey,
			Baseline,
			true);
		if (OperationKey == TEXT("boolean"))
			State->SetObjectField(TEXT("geometry"), Payload->GetObjectField(ExpectedBooleanStateField));
		if (OperationKey == TEXT("subdivide"))
		{
			State->SetNumberField(TEXT("triangle_count"),
				SubdividedTriangleCount(Payload, Baseline.TriangleCount));
		}
		return State;
	}

	TSharedPtr<FJsonObject> BuildActualVerificationState(
		const FString& OperationKey,
		const TSharedPtr<FJsonObject>& Payload,
		const FCarrierState& Baseline,
		const FCarrierState& Actual)
	{
		TSharedPtr<FJsonObject> State = BuildVerificationState(
			OperationKey,
			Baseline,
			SemanticMatches(
				OperationKey,
				Payload,
				Baseline,
				Actual));
		if (OperationKey == TEXT("boolean"))
			State->SetObjectField(TEXT("geometry"), BooleanGeometrySummary(Actual));
		if (OperationKey == TEXT("subdivide"))
		{
			State->SetNumberField(TEXT("triangle_count"), Actual.TriangleCount);
		}
		return State;
	}
}
