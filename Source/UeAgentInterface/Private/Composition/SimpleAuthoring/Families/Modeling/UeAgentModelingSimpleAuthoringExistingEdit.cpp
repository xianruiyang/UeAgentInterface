#include "Composition/SimpleAuthoring/Families/Modeling/UeAgentModelingSimpleAuthoringExistingEdit.h"

#include "Components/DynamicMeshComponent.h"
#include "Core/StateSemantic/UeAgentStateRevisionJson.h"
#include "DomainAdapters/ModelingGeometryEffect/UeAgentModelingGeometryEffectCapabilities.h"
#include "DomainAdapters/ModelingGeometryEffect/UeAgentModelingGeometryEffectBoundarySemantics.h"
#include "DomainAdapters/ModelingGeometryEffect/UeAgentModelingGeometryEffectCarrierRevision.h"
#include "DomainAdapters/ModelingGeometryEffect/UeAgentModelingGeometryEffectNormalObservation.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMeshActor.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescriptionToDynamicMesh.h"
#include "Misc/SecureHash.h"
#include "PhysicsEngine/BodySetup.h"
#include "Selections/MeshConnectedComponents.h"
#include "UDynamicMesh.h"

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

		bool TryReadInteger(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			int32& OutValue)
		{
			double Value = 0.0;
			if (!Object.IsValid()
				|| !Object->TryGetNumberField(Field, Value)
				|| !FMath::IsNearlyEqual(
					Value,
					FMath::RoundToDouble(Value)))
			{
				return false;
			}
			OutValue = FMath::RoundToInt(Value);
			return true;
		}

		bool TryReadNumberArray(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			const int32 Count,
			TArray<double>& OutValues)
		{
			OutValues.Reset();
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Object.IsValid()
				|| !Object->TryGetArrayField(Field, Values)
				|| !Values
				|| Values->Num() != Count)
			{
				return false;
			}
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				if (!Value.IsValid()
					|| Value->Type != EJson::Number)
				{
					OutValues.Reset();
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
			FCarrierState& OutState)
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
			return OutState.Bounds.IsValid
				&& OutState.VertexCount > 0
				&& OutState.TriangleCount > 0;
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

		bool TryReadNormalSplitTopology(
			const TSharedPtr<FJsonObject>& Baseline,
			FCarrierState& OutState)
		{
			const TSharedPtr<FJsonObject>* Topology = nullptr;
			if (!Baseline.IsValid())
			{
				return false;
			}
			if (!Baseline->HasField(NormalSplitTopologyField))
			{
				return true;
			}
			if (!Baseline->TryGetObjectField(
					NormalSplitTopologyField,
					Topology))
			{
				return false;
			}
			FString Schema;
			if (!Topology
				|| !Topology->IsValid()
				|| !(*Topology)->TryGetStringField(
					TEXT("schema"),
					Schema)
				|| Schema != NormalSplitTopologySchema
				|| !TryReadInteger(
					*Topology,
					TEXT("interior_edge_count"),
					OutState.NormalObservation.InteriorEdgeCount)
				|| !TryReadInteger(
					*Topology,
					TEXT("invalid_interior_edge_count"),
					OutState.NormalObservation.InvalidInteriorEdgeCount)
				|| !TryReadInteger(
					*Topology,
					TEXT("partially_split_interior_edge_count"),
					OutState.NormalObservation.
						PartiallySplitInteriorEdgeCount)
				|| !TryReadInteger(
					*Topology,
					TEXT("split_endpoint_count"),
					OutState.NormalSplitEndpointCount)
				|| !(*Topology)->TryGetStringField(
					TEXT("split_topology_sha1"),
					OutState.NormalSplitTopologyFingerprint)
				|| OutState.NormalSplitTopologyFingerprint.IsEmpty())
			{
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
			if (!Payload->TryGetObjectField(
					TEXT("direction"),
					DirectionObject)
				|| !DirectionObject
				|| !DirectionObject->IsValid()
				|| !TryReadNumberArray(
					*DirectionObject,
					TEXT("vector"),
					3,
					Vector))
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
				FString Operation = TEXT("union");
				Payload->TryGetStringField(
					TEXT("operation"),
					Operation);
				if (Operation == TEXT("union"))
				{
					return After.BoundaryEdgeCount == 0
						&& After.Bounds.Max.X
							>= Before.Bounds.Max.X + 20.0;
				}
				return StateChanged(Before, After)
					&& After.BoundaryEdgeCount == 0;
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
				const bool bTranslation =
					TryReadNumberArray(
						Payload,
						TEXT("translation_cm"),
						3,
						Translation);
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
				const int32 Levels =
					IntegerOr(Payload, TEXT("levels"), 1);
				int64 TriangleMultiplier = 1;
				for (int32 Level = 0; Level < Levels; ++Level)
				{
					TriangleMultiplier *= 4;
				}
				return Levels > 0
					&& After.TriangleCount
						>= Before.TriangleCount * TriangleMultiplier;
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
		return !TargetRef.IsEmpty()
			&& !DurablePackageName.IsEmpty()
			&& TargetRevision.IsSet()
			&& TargetRevision->IsValid()
			&& !CarrierKind.IsEmpty()
			&& (CarrierKind != TEXT("dynamic_mesh_actor")
				|| ActorGuid.IsValid())
			&& !GeometryFingerprint.IsEmpty()
			&& !BoundaryGeometryFingerprint.IsEmpty()
			&& Bounds.IsValid
			&& VertexCount > 0
			&& TriangleCount > 0;
	}

	bool TryReadCarrier(
		UWorld& World,
		IUeAgentModelingGeometryEffectReferenceResolver& Resolver,
		const FString& TargetRef,
		FCarrierState& OutState,
		FString& OutErrorCode,
		FString& OutErrorDetail)
	{
		OutState = FCarrierState();
		OutErrorCode.Reset();
		OutErrorDetail.Reset();
		UObject* Object = nullptr;
		FExecutionMutationError Error;
		if (TargetRef.IsEmpty()
			|| !Resolver.TryResolveReference(
				World,
				TargetRef,
				Object,
				Error)
			|| !Object)
		{
			OutErrorCode = Error.GetCode().IsEmpty()
				? TEXT("modeling_geometry_target_unresolved")
				: Error.GetCode();
			OutErrorDetail = Error.GetDetail();
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
			OutErrorCode = Error.GetCode();
			OutErrorDetail = Error.GetDetail();
			return false;
		}
		if (ADynamicMeshActor* Actor =
			Cast<ADynamicMeshActor>(Object))
		{
			OutState.ActorGuid = Actor->GetActorGuid();
			if (!OutState.ActorGuid.IsValid())
			{
				OutErrorCode =
					TEXT("modeling_geometry_actor_guid_invalid");
				OutErrorDetail = Actor->GetPathName();
				return false;
			}
			UDynamicMeshComponent* Component =
				Actor->GetDynamicMeshComponent();
			UDynamicMesh* DynamicMesh =
				Component ? Component->GetDynamicMesh() : nullptr;
			if (!DynamicMesh)
			{
				OutErrorCode =
					TEXT("modeling_geometry_dynamic_mesh_missing");
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
					bRead = ReadMeshState(Mesh, OutState);
				});
			if (!bRead)
			{
				OutErrorCode =
					TEXT("modeling_geometry_dynamic_mesh_read_failed");
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
				OutErrorCode =
					TEXT("modeling_geometry_static_mesh_source_missing");
				return false;
			}
			FDynamicMesh3 Mesh;
			FMeshDescriptionToDynamicMesh Converter;
			Converter.Convert(Description, Mesh);
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
			if (!ReadMeshState(Mesh, OutState))
			{
				OutErrorCode =
					TEXT("modeling_geometry_static_mesh_read_failed");
				return false;
			}
			return true;
		}

		OutErrorCode =
			TEXT("modeling_geometry_carrier_kind_unsupported");
		OutErrorDetail = Object->GetClass()->GetPathName();
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
		FString& OutErrorCode,
		FString& OutErrorDetail)
	{
		OutErrorCode.Reset();
		OutErrorDetail.Reset();
		if (!State.IsValid() || !Payload.IsValid())
		{
			OutErrorCode =
				TEXT("modeling_geometry_baseline_evidence_invalid");
			return false;
		}
		Payload->SetObjectField(
			BaselineField,
			BuildBaselineObject(State));
		Payload->SetStringField(
			DurablePackageNameField,
			State.DurablePackageName);
		if (OperationKey == TEXT("add_material_slot"))
		{
			FString MaterialRef;
			UObject* MaterialObject = nullptr;
			FExecutionMutationError Error;
			if (!Payload->TryGetStringField(
					TEXT("material_ref"),
					MaterialRef)
				|| !Resolver.TryResolveReference(
					World,
					MaterialRef,
					MaterialObject,
					Error)
				|| !Cast<UMaterialInterface>(MaterialObject))
			{
				OutErrorCode = Error.GetCode().IsEmpty()
					? TEXT(
						"modeling_geometry_material_reference_invalid")
					: Error.GetCode();
				OutErrorDetail = Error.GetDetail();
				return false;
			}
			Payload->SetStringField(
				ExpectedMaterialPathField,
				MaterialObject->GetPathName());
		}
		return true;
	}

	bool TryReadBaseline(
		const TSharedPtr<FJsonObject>& Payload,
		FCarrierState& OutState)
	{
		OutState = FCarrierState();
		const TSharedPtr<FJsonObject>* Baseline = nullptr;
		TArray<double> BoundsMin;
		TArray<double> BoundsMax;
		TArray<double> CollisionBoundsMin;
		TArray<double> CollisionBoundsMax;
		const TSharedPtr<FJsonObject>* TargetRevision = nullptr;
		bool bCollisionBoundsValid = false;
		double VertexChecksum = 0.0;
		if (!Payload.IsValid()
			|| !TryReadDurablePackageNameEvidence(
				Payload,
				OutState.DurablePackageName)
			|| !Payload->TryGetObjectField(
				BaselineField,
				Baseline)
			|| !Baseline
			|| !Baseline->IsValid()
			|| !(*Baseline)->TryGetStringField(
				TEXT("target_ref"),
				OutState.TargetRef)
			|| !(*Baseline)->TryGetObjectField(
				TEXT("target_revision"),
				TargetRevision)
			|| !TargetRevision
			|| !StateSemantic::FUeAgentStateRevisionJson::TryParse(
				*TargetRevision,
				OutState.TargetRevision)
			|| !(*Baseline)->TryGetStringField(
				TEXT("carrier_kind"),
				OutState.CarrierKind)
			|| !(*Baseline)->TryGetStringField(
				TEXT("geometry_fingerprint"),
				OutState.GeometryFingerprint)
			|| !TryReadInteger(
				*Baseline,
				TEXT("vertex_count"),
				OutState.VertexCount)
			|| !TryReadInteger(
				*Baseline,
				TEXT("triangle_count"),
				OutState.TriangleCount)
			|| !TryReadInteger(
				*Baseline,
				TEXT("edge_count"),
				OutState.EdgeCount)
			|| !TryReadInteger(
				*Baseline,
				TEXT("boundary_edge_count"),
				OutState.BoundaryEdgeCount)
			|| !TryReadInteger(
				*Baseline,
				TEXT("boundary_vertex_count"),
				OutState.BoundaryVertexCount)
			|| !(*Baseline)->TryGetStringField(
				TEXT("boundary_geometry_fingerprint"),
				OutState.BoundaryGeometryFingerprint)
			|| !(*Baseline)->TryGetNumberField(
				TEXT("vertex_checksum"),
				VertexChecksum)
			|| !TryReadInteger(
				*Baseline,
				TEXT("material_slot_count"),
				OutState.MaterialSlotCount)
			|| !TryReadInteger(
				*Baseline,
				TEXT("connected_component_count"),
				OutState.ConnectedComponentCount)
			|| !TryReadInteger(
				*Baseline,
				TEXT("collision_shape_count"),
				OutState.CollisionShapeCount)
			|| !TryReadInteger(
				*Baseline,
				TEXT("collision_box_count"),
				OutState.CollisionBoxCount)
			|| !TryReadInteger(
				*Baseline,
				TEXT("collision_sphere_count"),
				OutState.CollisionSphereCount)
			|| !TryReadInteger(
				*Baseline,
				TEXT("collision_capsule_count"),
				OutState.CollisionCapsuleCount)
			|| !TryReadInteger(
				*Baseline,
				TEXT("collision_convex_count"),
				OutState.CollisionConvexCount)
			|| !TryReadNumberArray(
				*Baseline,
				TEXT("bounds_min"),
				3,
				BoundsMin)
			|| !TryReadNumberArray(
				*Baseline,
				TEXT("bounds_max"),
				3,
				BoundsMax)
			|| !(*Baseline)->TryGetBoolField(
				TEXT("collision_bounds_valid"),
				bCollisionBoundsValid)
			|| !TryReadNumberArray(
				*Baseline,
				TEXT("collision_bounds_min"),
				3,
				CollisionBoundsMin)
			|| !TryReadNumberArray(
				*Baseline,
				TEXT("collision_bounds_max"),
				3,
				CollisionBoundsMax))
		{
			return false;
		}
		if (!TryReadNormalSplitTopology(*Baseline, OutState))
		{
			return false;
		}
		if (OutState.CarrierKind == TEXT("dynamic_mesh_actor"))
		{
			FString ActorGuid;
			if (!(*Baseline)->TryGetStringField(
					TEXT("actor_guid"),
					ActorGuid)
				|| !FGuid::Parse(ActorGuid, OutState.ActorGuid)
				|| !OutState.ActorGuid.IsValid())
			{
				return false;
			}
		}
		OutState.VertexChecksum =
			FMath::RoundToInt64(VertexChecksum);
		OutState.Bounds = FBox(
			FVector(BoundsMin[0], BoundsMin[1], BoundsMin[2]),
			FVector(BoundsMax[0], BoundsMax[1], BoundsMax[2]));
		if (bCollisionBoundsValid)
		{
			OutState.CollisionBounds = FBox(
				FVector(
					CollisionBoundsMin[0],
					CollisionBoundsMin[1],
					CollisionBoundsMin[2]),
				FVector(
					CollisionBoundsMax[0],
					CollisionBoundsMax[1],
					CollisionBoundsMax[2]));
		}
		return OutState.IsValid();
	}

	bool TryReadDurablePackageNameEvidence(
		const TSharedPtr<FJsonObject>& Payload,
		FString& OutPackageName)
	{
		OutPackageName.Reset();
		return Payload.IsValid()
			&& Payload->TryGetStringField(
				DurablePackageNameField,
				OutPackageName)
			&& !OutPackageName.IsEmpty();
	}

	TSharedPtr<FJsonObject> BuildExpectedVerificationState(
		const FString& OperationKey,
		const FCarrierState& Baseline)
	{
		return BuildVerificationState(
			OperationKey,
			Baseline,
			true);
	}

	TSharedPtr<FJsonObject> BuildActualVerificationState(
		const FString& OperationKey,
		const TSharedPtr<FJsonObject>& Payload,
		const FCarrierState& Baseline,
		const FCarrierState& Actual)
	{
		return BuildVerificationState(
			OperationKey,
			Baseline,
			SemanticMatches(
				OperationKey,
				Payload,
				Baseline,
				Actual));
	}
}
