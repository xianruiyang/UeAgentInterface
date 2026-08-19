#include "Composition/SimpleAuthoring/Families/Modeling/UeAgentModelingSimpleAuthoringExistingEdit.h"

#include "Components/DynamicMeshComponent.h"
#include "DomainAdapters/ModelingGeometryEffect/UeAgentModelingGeometryEffectCapabilities.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMeshActor.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescriptionToDynamicMesh.h"
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
		constexpr const TCHAR* ExpectedMaterialPathField =
			TEXT("__uai_expected_material_path");

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

		bool ReadMeshState(
			const FDynamicMesh3& Mesh,
			FCarrierState& OutState)
		{
			OutState.VertexCount = Mesh.VertexCount();
			OutState.EdgeCount = Mesh.EdgeCount();
			OutState.TriangleCount = Mesh.TriangleCount();
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
				TEXT("target_revision"),
				State.TargetRevision);
			Baseline->SetStringField(
				TEXT("carrier_kind"),
				State.CarrierKind);
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
			return Baseline;
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
				return After.NormalElementCount > 0
					&& After.NormalElementCount
						< After.TriangleCount * 3;
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
				return After.TriangleCount
						>= Before.TriangleCount + 8
					&& After.Bounds.Max.Z
						>= Before.Bounds.Max.Z
							+ FMath::Max(0.0, Distance - 5.0);
			}
			if (OperationKey == TEXT("fill_holes")
				|| OperationKey == TEXT("weld_edges"))
			{
				return After.BoundaryEdgeCount
					< Before.BoundaryEdgeCount;
			}
			if (OperationKey == TEXT("mesh_cut")
				|| OperationKey == TEXT("plane_cut"))
			{
				return After.BoundaryEdgeCount == 0
					&& After.TriangleCount > Before.TriangleCount;
			}
			if (OperationKey == TEXT("mirror"))
			{
				bool bKeepOriginal = true;
				Payload->TryGetBoolField(
					TEXT("keep_original"),
					bKeepOriginal);
				return After.BoundaryEdgeCount == 0
					&& (bKeepOriginal
						? After.TriangleCount
							>= Before.TriangleCount * 2
						: StateChanged(Before, After));
			}
			if (OperationKey == TEXT("offset_faces"))
			{
				const double Distance =
					NumberOr(
						Payload,
						TEXT("distance_cm"),
						0.0);
				return StateChanged(Before, After)
					&& (Distance <= 0.0
						|| After.Bounds.Max.Z
							>= Before.Bounds.Max.Z
								+ Distance - 5.0);
			}
			if (OperationKey == TEXT("remesh"))
			{
				return StateChanged(Before, After)
					&& After.TriangleCount
						>= Before.TriangleCount * 3;
			}
			if (OperationKey == TEXT("simplify"))
			{
				const double Ratio =
					NumberOr(
						Payload,
						TEXT("target_value"),
						1.0);
				return After.TriangleCount
					<= FMath::CeilToInt(
						Before.TriangleCount * Ratio)
						+ 1;
			}
			if (OperationKey == TEXT("subdivide"))
			{
				return After.TriangleCount
					>= Before.TriangleCount * 4;
			}
			if (OperationKey == TEXT("trim"))
			{
				return After.BoundaryEdgeCount == 0
					&& After.TriangleCount > 0
					&& StateChanged(Before, After);
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
			&& !TargetRevision.IsEmpty()
			&& !CarrierKind.IsEmpty()
			&& !GeometryFingerprint.IsEmpty()
			&& Bounds.IsValid
			&& VertexCount > 0
			&& TriangleCount > 0;
	}

	bool TryReadCarrier(
		UWorld& World,
		IUeAgentModelingGeometryEffectReferenceResolver& Resolver,
		const FString& TargetRef,
		const FString& TargetRevision,
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
			|| TargetRevision.IsEmpty()
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
		OutState.TargetRevision = TargetRevision;
		if (ADynamicMeshActor* Actor =
			Cast<ADynamicMeshActor>(Object))
		{
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
		Object->SetStringField(
			TEXT("target_revision"),
			State.TargetRevision);
		Object->SetStringField(
			TEXT("carrier_kind"),
			State.CarrierKind);
		Object->SetStringField(
			TEXT("geometry_fingerprint"),
			State.GeometryFingerprint);
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
		bool bCollisionBoundsValid = false;
		double VertexChecksum = 0.0;
		if (!Payload.IsValid()
			|| !Payload->TryGetObjectField(
				BaselineField,
				Baseline)
			|| !Baseline
			|| !Baseline->IsValid()
			|| !(*Baseline)->TryGetStringField(
				TEXT("target_ref"),
				OutState.TargetRef)
			|| !(*Baseline)->TryGetStringField(
				TEXT("target_revision"),
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
